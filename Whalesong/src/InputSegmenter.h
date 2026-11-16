// ------------------------------------------------------------------------------------------
// InputSegmenter - turns a hop-rate pitch stream into discrete notes.
//
// The pitch detector hands us one estimate every hop (46 ms by default): a MIDI number, or -1
// when the window was unpitched. That stream is not a melody. It stutters, it drops out in the
// middle of a held note, and perhaps one reading in twenty lands an octave away from the truth.
// This turns it into "you played a D4 here, for 300 ms, fairly hard".
//
// What it has to survive, and how:
//
//   Dropouts         A held note whose estimate goes unvoiced for a hop or two is still the
//                    same note. A gap only ends it once it has lasted offHops_.
//
//   Octave errors    YIN's classic failure is an estimate at half or twice the true frequency.
//                    Two defences: a reading an octave away from the note so far still counts
//                    as this note (so one bad reading cannot chop a note in two), and the
//                    note's pitch is the MODE of its readings, not the mean or the first. A
//                    minority of wrong readings loses the vote.
//
//   Repeated notes   Two D4s in a row are one unbroken pitch stream. Only the attack tells them
//                    apart, so the caller passes the loudest the envelope got since the last
//                    hop and a rise above where it sat starts a new note.
//
//   Noise            A "note" lasting one hop is the detector twitching, not playing. A note
//                    must survive minHops_ to be emitted at all.
//
// What it cannot do: a legato step of one or two semitones with no re-attack reads as one note
// bending, and a legato octave leap reads as one note (that is the price of the octave defence
// above). Struck and plucked material, which is most of what an MPC hears, re-attacks on every
// note and segments cleanly.
//
// No allocation after prepare(); safe to call from the audio thread.
// ------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cmath>

namespace ws
{

struct InputNote
{
    int64_t start    = 0;      // sample position of the attack
    int64_t length   = 0;      // samples from the attack to the release
    float   midi     = 0.0f;   // the settled pitch, a whole semitone
    float   velocity = 0.0f;   // 0..1, from the level at the attack
};

class InputSegmenter
{
public:
    static constexpr int kCapacity = 64;    // the most recent notes kept

    // --- tuning ---------------------------------------------------------------------------
    // A reading within this of the note so far (or of an octave of it) belongs to this note.
    // It has to stay under a semitone, or a step of one semitone reads as the same note still
    // sounding and the two merge - which is exactly what happened to a B4 running into a C5.
    // Vibrato is handled by kDisagreeHops below rather than by widening this.
    static constexpr float kSameTol = 0.9f;
    // How many consecutive readings must disagree before the note is called over. One bad
    // reading in the middle of a held note is not a new note.
    static constexpr int   kDisagreeHops = 2;
    // The peak since the last hop must stand this far above the level at the last hop to count
    // as an attack. This cannot be as high as instinct suggests. A note only decays as far as
    // it has time to: a sixteenth at 120 bpm is over in 125 ms, by which point a struck sound
    // has fallen to about two thirds, so the next attack is only a factor of 1.5 above it. The
    // threshold has to sit below that or fast playing reads as one long note. Going low is
    // safe in the other direction, because within a note the envelope DECAYS - the ratio there
    // is at or below 1, nowhere near this.
    static constexpr float kOnsetRise = 1.35f;
    // Votes are kept relative to the note's first reading, over this range either way.
    static constexpr int   kVoteSpan = 24;

    // windowSamples is how far back the pitch detector's analysis window reaches, in source
    // samples. The segmenter needs it to know when a reading has stopped describing the note
    // that just ended and started describing the one that just began.
    void prepare(float sampleRate, int hopSamples, int windowSamples)
    {
        sr_  = sampleRate > 0.0f ? sampleRate : 44100.0f;
        hop_ = hopSamples > 0 ? hopSamples : 2048;
        window_ = windowSamples > 0 ? windowSamples : hop_;

        const float hopSec = (float)hop_ / sr_;
        // A note must last this long to be real, and the detector must stay unvoiced this long
        // to end one. Both in hops, so they follow the hop rate rather than being fixed counts.
        //
        // The unvoiced count is only a backstop: a note that really stops is caught by the
        // level gate within a hop or two. So it can afford to be generous, and it has to be -
        // at 20% dropout, two unvoiced readings in a row inside a held note are common, and a
        // short count chops that note in half.
        minHops_ = (int)std::ceil(0.040f / hopSec);
        if (minHops_ < 2) minHops_ = 2;
        offHops_ = (int)std::ceil(0.150f / hopSec);
        if (offHops_ < 3) offHops_ = 3;

        // How far back an attack may be and still be taken as this note's. A patchy detector
        // can take several hops to name a pitch, and all that time the note is already
        // sounding; without a limit this generous those notes get dated from when the detector
        // caught up. 150 ms is past any plausible detector lag and short of a note at any sane
        // tempo, so it cannot reach back to the note before.
        backdateLimit_ = (int64_t)(0.150f * sr_);

        reset();
    }

    void reset()
    {
        open_ = false;
        gap_ = 0; disagree_ = 0; heldHops_ = 0;
        prevPeak_ = 0.0f;
        lastOnsetPos_ = 0;
        startPos_ = 0; endPos_ = 0; startVel_ = 0.0f;
        clearVotes();
        count_ = 0; head_ = 0; total_ = 0;
    }

    // Call once per detector hop.
    //   midi     the detector's estimate, or < 0 for an unpitched window
    //   peak     the loudest the input envelope got since the last hop
    //   peakPos  the sample position at which it got there
    //   gate     the level below which the input counts as silent
    //   pos      the sample position of this hop
    //
    // Level is deliberately NOT a parameter. The obvious thing to pass is the envelope follower
    // read at the hop, and it is wrong: a follower fast enough to catch an attack is faster
    // than the period of a low note, so on a bass line it tracks the waveform itself and reads
    // near zero at every zero crossing. Sampling that at the hop lands on an arbitrary point of
    // the cycle - a held E2 came back as 0.025, 0.0004, 0.000, 0.038 on consecutive hops, which
    // gates notes off mid-note and fires attacks that never happened. A peak held across the
    // whole hop spans two cycles even at the bottom of the detector's range, so it is steady,
    // and comparing one hop's peak with the last one's is a sound test for a rise.
    void hop(float midi, float peak, int64_t peakPos, float gate, int64_t pos)
    {
        const bool loud   = peak > gate;
        const bool voiced = midi > 0.0f;

        const bool onset = loud && peak > prevPeak_ * kOnsetRise;
        prevPeak_ = peak;

        // Remember where the last attack was. A note's pitch often takes a hop longer to
        // resolve than its attack takes to arrive - the detector needs most of a window of the
        // new note first - and without this the note would be dated from when we noticed it
        // rather than from when it was played.
        if (onset) lastOnsetPos_ = peakPos;

        if (open_)
        {
            bool agrees = false;
            if (voiced) agrees = belongs(midi);

            if (!loud)
            {
                close(pos);
            }
            else if (onset)
            {
                // A fresh attack ends the note whatever the pitch says.
                close(pos);
            }
            else if (!voiced)
            {
                // A hole in the middle of a held note. Tolerated up to offHops_.
                if (++gap_ >= offHops_) close(pos);
                else { ++heldHops_; endPos_ = pos; }
            }
            else if (agrees)
            {
                addVote(midi);
                gap_ = 0; disagree_ = 0;
                ++heldHops_; endPos_ = pos;
            }
            else
            {
                // A different pitch. Wait for it to persist before believing it, so that one
                // wild reading does not split a note.
                if (++disagree_ >= kDisagreeHops) close(pos);
                else { ++heldHops_; endPos_ = pos; }
            }
        }

        // Do not open a note on a reading that cannot yet be about it.
        //
        // The detector's window looks backwards. For the first window's worth of a new note,
        // what it measures is still mostly the OLD note, so the estimate it returns is the
        // pitch that has just finished. Open on that and you get a phantom note of the previous
        // pitch at every boundary, which is then chopped a couple of hops later when the real
        // pitch arrives - one note in, two notes out. So after an attack, wait until a full
        // window has passed before believing the pitch. Nothing is lost by waiting: the attack
        // position is remembered, so the note is still dated from where it was played.
        const bool settled = lastOnsetPos_ <= 0 || (pos - lastOnsetPos_) >= (int64_t)window_;

        if (!open_ && loud && voiced && settled)
        {
            open_ = true;
            // Back-date to the attack when there was one recently enough to be this note's.
            // Failing that the note began somewhere in the hop just finished, and the start of
            // that hop is a better guess than its end.
            if (lastOnsetPos_ > 0 && (pos - lastOnsetPos_) <= backdateLimit_)
                startPos_ = lastOnsetPos_;
            else
                startPos_ = pos - hop_;
            if (startPos_ < 0) startPos_ = 0;
            endPos_ = pos;
            gap_ = 0; disagree_ = 0; heldHops_ = 1;
            clearVotes();
            base_ = (int)std::lround(midi);
            addVote(midi);
            startVel_ = levelToVelocity(peak);
        }
    }

    // Close anything still open, e.g. when the transport stops.
    void flush(int64_t pos) { if (open_) close(pos); }

    int minHops() const { return minHops_; }
    int offHops() const { return offHops_; }

    int count() const { return count_; }           // notes kept, oldest first
    int64_t total() const { return total_; }       // notes ever emitted

    const InputNote& note(int i) const
    {
        const int idx = (head_ - count_ + i + 2 * kCapacity) % kCapacity;
        return notes_[idx];
    }
    // Newest first: recent(0) is the note that just finished.
    const InputNote& recent(int i) const { return note(count_ - 1 - i); }

    bool noteOpen() const { return open_; }

private:
    // ---- the note being built --------------------------------------------------------------

    void clearVotes()
    {
        for (int i = 0; i < 2 * kVoteSpan + 1; ++i) votes_[i] = 0;
        best_ = kVoteSpan;
        base_ = 0;
    }

    void addVote(float midi)
    {
        const int d = (int)std::lround(midi) - base_;
        if (d < -kVoteSpan || d > kVoteSpan) return;
        const int i = d + kVoteSpan;
        ++votes_[i];
        if (votes_[i] > votes_[best_]) best_ = i;
    }

    int modePitch() const { return base_ + best_ - kVoteSpan; }

    // Does this reading belong to the note so far? Either it is close to the mode, or it is an
    // octave relative of it - which is what an octave error looks like.
    bool belongs(float midi) const
    {
        const float d = std::fabs(midi - (float)modePitch());
        if (d <= kSameTol) return true;
        const float oct = d - 12.0f * std::floor(d / 12.0f + 0.5f);
        return std::fabs(oct) <= kSameTol && d <= (float)kVoteSpan;
    }

    void close(int64_t pos)
    {
        open_ = false;
        if (heldHops_ < minHops_) return;         // detector twitch, not a note

        InputNote n;
        n.start    = startPos_;
        n.length   = (pos > startPos_ ? pos : endPos_ + hop_) - startPos_;
        n.midi     = (float)modePitch();
        n.velocity = startVel_;

        notes_[head_] = n;
        head_ = (head_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
        ++total_;
    }

    float levelToVelocity(float level) const
    {
        if (level <= 1.0e-6f) return 0.05f;
        const float db = 20.0f * std::log10(level);
        float v = (db + 48.0f) / 42.0f;
        if (v < 0.05f) v = 0.05f;
        if (v > 1.0f)  v = 1.0f;
        return v;
    }

    float sr_ = 44100.0f;
    int     hop_ = 2048, window_ = 2048;
    int64_t backdateLimit_ = 6615;
    int   minHops_ = 2, offHops_ = 2;

    bool    open_ = false;
    int     gap_ = 0, disagree_ = 0, heldHops_ = 0;
    int64_t startPos_ = 0, endPos_ = 0;
    float   startVel_ = 0.0f, prevPeak_ = 0.0f;
    int64_t lastOnsetPos_ = 0;

    int base_ = 0, best_ = kVoteSpan;
    int votes_[2 * kVoteSpan + 1] = {0};

    InputNote notes_[kCapacity];
    int     head_ = 0, count_ = 0;
    int64_t total_ = 0;
};

} // namespace ws
