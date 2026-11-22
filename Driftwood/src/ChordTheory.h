// ChordTheory.h - Driftwood's music theory: scales, the chord on each scale degree, voicings and
// voice leading. Pure functions, no state, no allocation: called on note-on from the audio thread.
//
// SPDX-License-Identifier: MIT

#ifndef DRIFTWOOD_CHORDTHEORY_H
#define DRIFTWOOD_CHORDTHEORY_H

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace driftwood {

constexpr int kMaxTones = 5;            // chord tones (root, 3rd, 5th, 7th, 9th); Bass adds one voice
constexpr int kMaxChordVoices = kMaxTones + 1;

enum Scale   { kMajor = 0, kMinor, kDorian, kPhrygian, kLydian, kMixolydian, kHarmMinor, kMelMinor, kNumScales };
enum Quality { kTriad = 0, kSeventh, kNinth, kSus2, kSus4, kAdd9, kNumQualities };
enum Voicing { kClose = 0, kOpen, kSpread };
enum NoteMap { kMapKeys = 0, kMapPads };
enum OffKey  { kSnap = 0, kOwnChord };

// Semitones of each scale step above the tonic.
static const int kScaleSteps[kNumScales][7] = {
    {0, 2, 4, 5, 7, 9, 11},   // Major (Ionian)
    {0, 2, 3, 5, 7, 8, 10},   // Natural minor (Aeolian)
    {0, 2, 3, 5, 7, 9, 10},   // Dorian
    {0, 1, 3, 5, 7, 8, 10},   // Phrygian
    {0, 2, 4, 6, 7, 9, 11},   // Lydian
    {0, 2, 4, 5, 7, 9, 10},   // Mixolydian
    {0, 2, 3, 5, 7, 8, 11},   // Harmonic minor
    {0, 2, 3, 5, 7, 9, 11},   // Melodic minor (ascending)
};

constexpr int kPadBaseNote = 36;        // Pads map: note 36 (MPC pad 1, bank A) is degree I

inline int floorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }
inline int mod(int a, int b) { int m = a % b; return m < 0 ? m + b : m; }

// Semitones of scale step s above the tonic; s may be negative or > 6 (other octaves).
inline int scaleStep(int scale, int s)
{
    const int o = floorDiv(s, 7);
    return kScaleSteps[scale][s - 7 * o] + 12 * o;
}

// Scale degree (0..6) of a pitch class relative to the key, or -1 if it is not in the scale.
inline int degreeOf(int scale, int rel)
{
    for (int d = 0; d < 7; ++d) if (kScaleSteps[scale][d] == rel) return d;
    return -1;
}

// A chord to play: an absolute root pitch and the intervals of its tones above it, in chord-tone
// order (root, 3rd, 5th, 7th, 9th; sus chords put the 2nd or 4th in the 3rd's place).
struct Chord
{
    int root = 60;
    int n = 0;
    int iv[kMaxTones] = {0, 0, 0, 0, 0};
    int degree = 0;         // 0..6, or -1 for an out-of-key chord played as its own chord
};

// The diatonic chord on degree d: stacked thirds taken from the scale. Sus chords are true sus
// chords (root, major 2nd or perfect 4th, perfect 5th) so that they are always consonant; on the
// degree whose diatonic 5th is diminished (vii in major) that means one note from outside the key.
inline void diatonicIntervals(int scale, int d, int quality, Chord& c)
{
    const int r = scaleStep(scale, d);
    auto st = [&](int k) { return scaleStep(scale, d + k) - r; };
    switch (quality)
    {
        default:
        case kTriad:   c.n = 3; c.iv[0] = 0; c.iv[1] = st(2); c.iv[2] = st(4); break;
        case kSeventh: c.n = 4; c.iv[0] = 0; c.iv[1] = st(2); c.iv[2] = st(4); c.iv[3] = st(6); break;
        case kNinth:   c.n = 5; c.iv[0] = 0; c.iv[1] = st(2); c.iv[2] = st(4); c.iv[3] = st(6); c.iv[4] = st(8); break;
        case kSus2:    c.n = 3; c.iv[0] = 0; c.iv[1] = 2; c.iv[2] = 7; break;
        case kSus4:    c.n = 3; c.iv[0] = 0; c.iv[1] = 5; c.iv[2] = 7; break;
        case kAdd9:    c.n = 4; c.iv[0] = 0; c.iv[1] = st(2); c.iv[2] = st(4); c.iv[3] = st(8); break;
    }
}

// An out-of-key note played as "its own chord": the major-family chord on that root (major triad,
// dominant 7th, dominant 9th, sus2, sus4, add9). That gives the borrowed chords (bVII, bVI, bIII in
// a major key) and the secondary dominants (II7, III7, VI7) their usual sound.
inline void ownIntervals(int quality, Chord& c)
{
    static const int kOwn[kNumQualities][kMaxTones] = {
        {0, 4, 7, 0, 0}, {0, 4, 7, 10, 0}, {0, 4, 7, 10, 14}, {0, 2, 7, 0, 0}, {0, 5, 7, 0, 0}, {0, 4, 7, 14, 0}};
    static const int kN[kNumQualities] = {3, 4, 5, 3, 3, 4};
    const int q = (quality >= 0 && quality < kNumQualities) ? quality : 0;
    c.n = kN[q];
    for (int i = 0; i < kMaxTones; ++i) c.iv[i] = kOwn[q][i];
}

// Which chord does this incoming note play?
//   Keys map: the note's pitch class picks the scale degree, and the chord's root is the note itself
//             (or the nearest scale note when Off-Key = Snap; a tie snaps down).
//   Pads map: every note is the next scale degree: 36 = I, 37 = ii, ... 42 = vii, 43 = I an octave up,
//             and so on across the 16 pads (36..51 = I..vii, I'..vii', I'', ii''). Notes below 36 go
//             down the scale. Degree I of pad 1 sits at the tonic at or just below Register.
inline Chord resolveChord(int note, int key, int scale, int quality, int map, int offKey, int registerNote)
{
    Chord c;
    if (map == kMapPads)
    {
        const int idx = note - kPadBaseNote;
        const int tonic = registerNote - mod(registerNote - key, 12);   // tonic at or below Register
        c.degree = mod(idx, 7);
        c.root = tonic + scaleStep(scale, idx);
        diatonicIntervals(scale, c.degree, quality, c);
        return c;
    }
    const int rel = mod(note - key, 12);
    int d = degreeOf(scale, rel);
    if (d >= 0)
    {
        c.degree = d; c.root = note;
        diatonicIntervals(scale, d, quality, c);
        return c;
    }
    if (offKey == kOwnChord)
    {
        c.degree = -1; c.root = note;
        ownIntervals(quality, c);
        return c;
    }
    for (int dist = 1; dist <= 6; ++dist)
    {
        if ((d = degreeOf(scale, mod(rel - dist, 12))) >= 0) { c.root = note - dist; break; }
        if ((d = degreeOf(scale, mod(rel + dist, 12))) >= 0) { c.root = note + dist; break; }
    }
    c.degree = d < 0 ? 0 : d;
    diatonicIntervals(scale, c.degree, quality, c);
    return c;
}

inline void sortPitches(int* p, int n)
{
    for (int i = 1; i < n; ++i) { int k = p[i], j = i - 1; while (j >= 0 && p[j] > k) { p[j + 1] = p[j]; --j; } p[j + 1] = k; }
}

// Voice a chord with its root at rootPitch. Inversion k raises the k lowest chord tones (in chord-tone
// order root, 3rd, 5th, 7th) by an octave, so on a triad "3rd" is root position an octave up.
//   Close   the tones as stacked (within an octave, plus the 9th above it).
//   Open    drop-2: the second-highest note of the close voicing goes down an octave.
//   Spread  every other note of the close voicing (2nd, 4th from the bottom) goes up an octave.
// Returns the number of notes; out[] is sorted low to high.
inline int voiceChord(const Chord& c, int rootPitch, int inversion, int voicing, int* out)
{
    const int n = c.n;
    const int inv = inversion < 0 ? 0 : (inversion > 3 ? 3 : inversion);
    for (int i = 0; i < n; ++i) out[i] = rootPitch + c.iv[i] + (i < inv && i < 4 ? 12 : 0);
    sortPitches(out, n);
    if (voicing == kOpen && n >= 3) { out[n - 2] -= 12; sortPitches(out, n); }
    else if (voicing == kSpread && n >= 3) { for (int i = 1; i < n; i += 2) out[i] += 12; sortPitches(out, n); }
    return n;
}

// How far a change of chord moves the hands: every new note to its nearest old note, plus every old
// note to its nearest new note (so it also works when the chords have different sizes).
inline int movement(const int* a, int na, const int* b, int nb)
{
    int cost = 0;
    for (int i = 0; i < nb; ++i) { int m = 1000; for (int j = 0; j < na; ++j) m = std::min(m, std::abs(b[i] - a[j])); cost += m; }
    for (int j = 0; j < na; ++j) { int m = 1000; for (int i = 0; i < nb; ++i) m = std::min(m, std::abs(b[i] - a[j])); cost += m; }
    return cost;
}

// Voice leading: try every inversion of the chord in every octave whose centre stays within
// +-7 semitones of Register, and keep the one that moves least from the previous chord (a small
// pull towards Register stops a long progression drifting to one end of the range). With no
// previous chord, take the selected inversion nearest to Register.
inline int leadChord(const Chord& c, int voicing, int inversion, int registerNote, const int* prev, int nPrev, int* out)
{
    int best[kMaxTones] = {0, 0, 0, 0, 0};
    float bestCost = 1.0e9f;
    int cand[kMaxTones];
    const int maxInv = c.n >= 4 ? 4 : c.n;
    for (int inv = 0; inv < maxInv; ++inv)
    {
        if (nPrev == 0 && inv != (inversion % maxInv)) continue;
        for (int o = -5; o <= 5; ++o)
        {
            const int root = c.root + 12 * o;
            voiceChord(c, root, inv, voicing, cand);
            const float centre = 0.5f * (float)(cand[0] + cand[c.n - 1]);
            const float off = std::fabs(centre - (float)registerNote);
            if (off > 7.0f) continue;
            const float cost = (nPrev > 0 ? (float)movement(prev, nPrev, cand, c.n) : 0.0f) + 0.25f * off;
            if (cost < bestCost) { bestCost = cost; for (int i = 0; i < c.n; ++i) best[i] = cand[i]; }
        }
    }
    if (bestCost >= 1.0e8f) return voiceChord(c, c.root, inversion, voicing, out);   // cannot happen: a window of 15 always fits
    for (int i = 0; i < c.n; ++i) out[i] = best[i];
    return c.n;
}

// Bass: the chord's root below the bottom of the voicing: "-1 Oct" puts it 1..12 semitones under the
// lowest note (an octave under it when the root is the lowest note), "-2 Oct" another octave down.
inline int bassNote(const Chord& c, const int* voiced, int octaves)
{
    const int lowest = voiced[0];
    const int r0 = lowest - mod(lowest - c.root, 12);   // highest root pitch at or below the lowest note
    int b = r0 - 12 * (r0 == lowest ? octaves : octaves - 1);
    while (b < 24) b += 12;
    return b;
}

} // namespace driftwood

#endif // DRIFTWOOD_CHORDTHEORY_H
