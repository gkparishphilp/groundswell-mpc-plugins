// ChordTones.h - which notes Conch's resonators ring: chord qualities, diatonic chords, voicing.
//
// SPDX-License-Identifier: MIT
//
// Pure functions, no state, nothing allocated: safe on the audio thread.
//
// Voicing rule: a chord is not stacked from its root. Every note between the Low Note and Low Note +
// Spread octaves whose pitch class is in the chord gets a resonator (below C3 only the root and the
// fifth, so the low end stays clear). That way two chords that share a note share the very same
// resonator pitch, and a chord change moves as few notes as possible.

#pragma once

#include "Harmony.h"   // kScales, kNumScales

namespace conch {

// Chord qualities, as semitones above the root. The order is the order of the Quality parameter's
// choices and of the Chord meter's names. Never reorder; append.
struct Quality { const char* name; const char* suffix; int n; int iv[5]; };
static const Quality kQualities[] = {
    {"Major", "",     3, {0, 4, 7, 0, 0}},
    {"Minor", "m",    3, {0, 3, 7, 0, 0}},
    {"Sus2",  "sus2", 3, {0, 2, 7, 0, 0}},
    {"Sus4",  "sus4", 3, {0, 5, 7, 0, 0}},
    {"Power", "5",    2, {0, 7, 0, 0, 0}},
    {"Dim",   "dim",  3, {0, 3, 6, 0, 0}},
    {"Aug",   "aug",  3, {0, 4, 8, 0, 0}},
    {"Maj7",  "maj7", 4, {0, 4, 7, 11, 0}},
    {"Min7",  "m7",   4, {0, 3, 7, 10, 0}},
    {"Dom7",  "7",    4, {0, 4, 7, 10, 0}},
    {"m7b5",  "m7b5", 4, {0, 3, 6, 10, 0}},
    {"Maj9",  "maj9", 5, {0, 4, 7, 11, 14}},
    {"Min9",  "m9",   5, {0, 3, 7, 10, 14}},
    {"Dom9",  "9",    5, {0, 4, 7, 10, 14}},
};
constexpr int kNumQualities = (int)(sizeof(kQualities) / sizeof(kQualities[0]));

// Tones parameter: how much of a diatonic chord to ring.
enum Tones { kTriad = 0, kSeventh, kNinth, kFifths, kNumTones };

struct Chord
{
    int root = 9;                 // pitch class 0..11
    int n = 3;                    // number of tones
    int iv[5] = {0, 3, 7, 0, 0};  // semitones above the root
};

inline int pcMod(int v) { return ((v % 12) + 12) % 12; }

inline int pcMask(const int* iv, int n)
{
    int m = 0;
    for (int i = 0; i < n; ++i) m |= 1 << pcMod(iv[i]);
    return m;
}

// Which named quality these intervals are (as pitch-class sets), for the Chord meter.
inline int qualityOf(const Chord& c)
{
    const int mask = pcMask(c.iv, c.n);
    for (int q = 0; q < kNumQualities; ++q)
        if (pcMask(kQualities[q].iv, kQualities[q].n) == mask) return q;
    if (c.n >= 3)                     // name it by its triad
    {
        const int tri = pcMask(c.iv, 3);
        for (int q = 0; q < kNumQualities; ++q)
            if (kQualities[q].n == 3 && pcMask(kQualities[q].iv, 3) == tri) return q;
    }
    return 4;                         // two notes: call it a fifth
}

inline Chord chordFromQuality(int root, int q)
{
    Chord c;
    const Quality& k = kQualities[q < 0 ? 0 : (q >= kNumQualities ? kNumQualities - 1 : q)];
    c.root = pcMod(root);
    c.n = k.n;
    for (int i = 0; i < 5; ++i) c.iv[i] = k.iv[i];
    return c;
}

// Semitones from scale degree d up k scale steps (k may exceed 6).
inline int degreeSpan(int scaleIdx, int d, int k)
{
    const ScaleDef& sc = kScales[scaleIdx];
    const int idx = d + k;
    return sc.notes[idx % 7] + 12 * (idx / 7) - sc.notes[d];
}

// The chord on scale degree `degree` (0..6) of a key, stacked in thirds from the scale.
// A ninth that would be a minor ninth (13 semitones) is left out: it clashes in a resonator chord.
inline Chord diatonicChord(int keyRoot, int scaleIdx, int degree, int tones)
{
    Chord c;
    scaleIdx = scaleIdx < 0 ? 0 : (scaleIdx >= kNumScales ? kNumScales - 1 : scaleIdx);
    degree = ((degree % 7) + 7) % 7;
    c.root = pcMod(keyRoot + kScales[scaleIdx].notes[degree]);
    c.n = 0;
    if (tones == kFifths)
    {
        c.iv[c.n++] = 0;
        c.iv[c.n++] = degreeSpan(scaleIdx, degree, 4);
        return c;
    }
    const int steps = tones == kNinth ? 5 : (tones == kSeventh ? 4 : 3);
    for (int s = 0; s < steps; ++s)
    {
        const int semis = degreeSpan(scaleIdx, degree, 2 * s);
        if (s == 4 && semis == 13) continue;
        c.iv[c.n++] = semis;
    }
    return c;
}

// The MIDI notes the resonators ring, ascending. Returns how many (at most maxN).
constexpr int kThinBelow = 48;   // C3: below it only the root and the fifth
constexpr int kHighest = 96;     // C7: nothing above

inline int voiceChord(const Chord& c, int lowNote, int octaves, int* out, int maxN)
{
    const int full = pcMask(c.iv, c.n);
    int thin = 1 << c.root;
    for (int i = 0; i < c.n; ++i)
    {
        const int iv = pcMod(c.iv[i]);
        if (iv == 7 || iv == 6 || iv == 8) thin |= 1 << pcMod(c.root + iv);
    }
    // `full` holds intervals; turn it into absolute pitch classes.
    int fullPc = 0;
    for (int pc = 0; pc < 12; ++pc) if (full & (1 << pc)) fullPc |= 1 << pcMod(c.root + pc);

    int n = 0;
    const int hi = lowNote + 12 * octaves;
    for (int m = lowNote; m < hi && m <= kHighest && n < maxN; ++m)
    {
        const int mask = (m < kThinBelow) ? thin : fullPc;
        if (mask & (1 << pcMod(m))) out[n++] = m;
    }
    return n;
}

} // namespace conch
