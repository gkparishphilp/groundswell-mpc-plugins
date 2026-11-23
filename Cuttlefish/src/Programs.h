// Programs.h - Cuttlefish's built-in programs (included inside namespace cuttlefish by Cuttlefish.cpp).
//
// SPDX-License-Identifier: MIT
//
// Each program starts from every parameter's default and overrides a few. Values are plain values:
// percentages, milliseconds, Hz, or the index of a choice.

inline void Cuttlefish::addPrograms()
{
    auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> o)
    {
        std::vector<float> v((size_t)numParams());
        for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
        for (const auto& x : o) v[(size_t)x.first] = x.second;
        addProgram(name, std::move(v));
    };
    const float divBar1 = 3, div2Bars = 2, div4Bars = 1, div8 = 10, div16 = 12;

    // A plain saw through an open filter: the starting point.
    program("Init", {});

    // Vowels and random spectra drifting together; Colour pushes the two oscillators apart.
    program("Ink Cloud Pad", {{pTable1, kTabVowel}, {pMorph1, 20}, {pTable2, kTabSpectra}, {pMorph2, 30},
        {pLevel1, 70}, {pLevel2, 55}, {pUnison, 3}, {pDetune, 35}, {pWidth, 80}, {pDrift, 35},
        {pAttack, 900}, {pDecay, 2000}, {pSustain, 80}, {pRelease, 2500},
        {pCutoff, 3500}, {pReso, 15}, {pKeyTrack, 30}, {pEnvAmt, 10}, {pFAttack, 1500}, {pFSustain, 60},
        {pL2Mode, kLfoFree}, {pL2Rate, 0.12f}, {pL2Shape, kShSmooth}, {pAmt2, 30},
        {pColMorph1, 60}, {pColMorph2, -40}, {pColFilter, 35},
        {pChorus, 40}, {pChMode, Chorus::kEnsemble}, {pDelay, 15}, {pReverb, 45}, {pRevSize, 70},
        {pVoices, 10}, {pVelLevel, 40}, {pOutput, -2}});

    // Synced saw with a swept sync envelope, legato glide and mod-wheel vibrato that fades in.
    program("Chromatophore Lead", {{pMorph1, 50}, {pWarpMode1, kWSync}, {pWarp1, 25}, {pMorph2, 70}, {pLevel2, 45},
        {pFine2, 10}, {pPlayMode, kLegato}, {pGlide, 90}, {pUnison, 2}, {pDetune, 20}, {pWidth, 40},
        {pCutoff, 2500}, {pReso, 30}, {pEnvAmt, 35}, {pFDecay, 600}, {pFSustain, 30},
        {pSrc3, kSrcModEnv}, {pDst3, kDstWarp1}, {pAmt3, 40}, {pMDecay, 500},
        {pL1Mode, kLfoRetrig}, {pL1Rate, 5.5f}, {pL1Fade, 400},
        {pColMorph1, 30}, {pColMorph2, 20}, {pColFilter, 40},
        {pDelay, 25}, {pDlyTime, 7}, {pReverb, 15}, {pRelease, 300}, {pVoices, 1}});

    // Glassy partials that close as the note dies; an octave sine underneath.
    program("Reef Pluck", {{pTable1, kTabGlass}, {pMorph1, 30}, {pMorph2, 0}, {pOct2, 1}, {pLevel2, 30},
        {pAttack, 1}, {pDecay, 450}, {pSustain, 0}, {pRelease, 400},
        {pEnvMorph, 60}, {pMDecay, 250}, {pMSustain, 0},
        {pFilterMode, kLP12}, {pCutoff, 1200}, {pEnvAmt, 45}, {pFDecay, 300}, {pFSustain, 0}, {pVelFilter, 40},
        {pPhase, 1}, {pVelLevel, 60}, {pDelay, 30}, {pDlyTime, 7}, {pReverb, 25}, {pColMorph1, 60}, {pColFilter, 40}});

    // Saw and square with a sine sub, driven LP24, legato glide.
    program("Deep Ink Bass", {{pMorph1, 50}, {pMorph2, 75}, {pLevel2, 50}, {pOct2, -1}, {pSub, 60},
        {pFilterMode, kLP24}, {pCutoff, 280}, {pReso, 25}, {pDrive, 30}, {pEnvAmt, 45}, {pFDecay, 250},
        {pFSustain, 15}, {pKeyTrack, 40}, {pAttack, 1}, {pDecay, 800}, {pSustain, 80}, {pRelease, 120},
        {pPlayMode, kLegato}, {pGlide, 60}, {pPhase, 1}, {pVoices, 1}, {pDrift, 5},
        {pColMorph1, 40}, {pColMorph2, 20}, {pColFilter, 50}, {pOutput, -4}});

    // Electric-piano tine: velocity barks the tine, chorus.
    program("Tidal Keys", {{pTable1, kTabTine}, {pMorph1, 25}, {pMorph2, 0}, {pLevel2, 35},
        {pAmt4, 45}, {pDecay, 1800}, {pSustain, 20}, {pRelease, 600},
        {pFilterMode, kLP12}, {pCutoff, 6000}, {pKeyTrack, 50}, {pVelFilter, 30},
        {pChorus, 35}, {pChRate, 0.4f}, {pReverb, 20}, {pDrift, 15}, {pVelLevel, 75}, {pColMorph1, 50}, {pColFilter, 25}});

    // The colour change: song-locked LFOs walk both morphs, random warp per note, wide and drifting.
    program("Colour Shift", {{pTable1, kTabMetal}, {pMorph1, 20}, {pTable2, kTabFormant}, {pMorph2, 40},
        {pLevel1, 60}, {pLevel2, 55}, {pWarpMode2, kWFM}, {pWarp2, 15},
        {pL1Mode, kLfoSync}, {pL1Div, div2Bars}, {pL1Shape, kShSmooth},
        {pL2Mode, kLfoSync}, {pL2Div, div4Bars}, {pL2Shape, kShTri},
        {pSrc1, kSrcLfo1}, {pDst1, kDstMorph1}, {pAmt1, 35},
        {pSrc2, kSrcLfo2}, {pDst2, kDstMorph2}, {pAmt2, 40},
        {pSrc3, kSrcLfo2}, {pDst3, kDstPan}, {pAmt3, 30},
        {pSrc4, kSrcRandom}, {pDst4, kDstWarp2}, {pAmt4, 30},
        {pUnison, 2}, {pWidth, 90}, {pDrift, 60}, {pAttack, 1200}, {pSustain, 90}, {pRelease, 3000},
        {pFilterMode, kLP12}, {pCutoff, 4000}, {pReso, 20},
        {pDelay, 30}, {pDlyTime, 10}, {pDlyFdbk, 50}, {pReverb, 50}, {pRevSize, 80}, {pChorus, 20},
        {pVoices, 10}, {pVelLevel, 40}});

    // A voice sliding through its vowels; vibrato fades in.
    program("Siren Vowels", {{pTable1, kTabVowel}, {pMorph1, 50}, {pTable2, kTabVowel}, {pMorph2, 25}, {pLevel2, 40},
        {pFine2, -8}, {pL2Mode, kLfoFree}, {pL2Rate, 0.25f}, {pL2Shape, kShTri}, {pAmt2, 45},
        {pL1Mode, kLfoRetrig}, {pL1Rate, 5.0f}, {pL1Fade, 600}, {pAmt1, 20},
        {pAttack, 250}, {pRelease, 900}, {pUnison, 3}, {pDetune, 18}, {pWidth, 70},
        {pFilterMode, kLP12}, {pCutoff, 8000}, {pChorus, 30}, {pChMode, Chorus::kEnsemble}, {pReverb, 35},
        {pVoices, 10}, {pColMorph1, 50}, {pColMorph2, 50}, {pColFilter, 0}});

    // Glass partials phase-modulated by a sine two octaves up; bright strike, mellow ring.
    program("Glass Bells", {{pTable1, kTabGlass}, {pMorph1, 20}, {pWarpMode1, kWFM}, {pWarp1, 20},
        {pMorph2, 0}, {pOct2, 2}, {pLevel2, 25},
        {pAttack, 1}, {pDecay, 3000}, {pSustain, 0}, {pRelease, 2500},
        {pEnvMorph, 60}, {pMDecay, 1200}, {pMSustain, 0}, {pPhase, 1},
        {pReverb, 45}, {pRevSize, 75}, {pDelay, 20}, {pDlyTime, 8}, {pVelLevel, 80}, {pColMorph1, 40}, {pColFilter, 0}});

    // Drawbar registrations on Morph (and Colour), ensemble chorus as a rotary hint.
    program("Sunken Organ", {{pTable1, kTabOrgan}, {pMorph1, 55}, {pColMorph1, 45}, {pColMorph2, 0}, {pColFilter, 0},
        {pAttack, 3}, {pDecay, 100}, {pSustain, 100}, {pRelease, 60},
        {pFilterMode, kLP12}, {pChorus, 55}, {pChMode, Chorus::kEnsemble}, {pChRate, 5.5f}, {pChDepth, 30},
        {pDrift, 15}, {pReverb, 20}, {pVelLevel, 0}, {pOutput, -4}});

    // A brass swell: the morph envelope opens the horn, velocity adds bite.
    program("Kelp Brass", {{pTable1, kTabBrass}, {pMorph1, 15}, {pTable2, kTabBrass}, {pMorph2, 25}, {pFine2, 8}, {pLevel2, 60},
        {pEnvMorph, 70}, {pMAttack, 120}, {pMDecay, 700}, {pMSustain, 50},
        {pUnison, 2}, {pDetune, 15}, {pWidth, 50},
        {pAttack, 40}, {pDecay, 500}, {pSustain, 85}, {pRelease, 250},
        {pCutoff, 5000}, {pVelFilter, 35}, {pAmt4, 25}, {pChorus, 15}, {pReverb, 25}, {pColMorph1, 40}, {pColFilter, 30}});

    // Two detuned pulse oscillators with LFO pulse-width modulation.
    program("PWM Strings", {{pTable1, kTabPWM}, {pMorph1, 20}, {pTable2, kTabPWM}, {pMorph2, 40}, {pFine2, -9}, {pLevel2, 70},
        {pSrc1, kSrcLfo1}, {pDst1, kDstMorphBoth}, {pAmt1, 25}, {pL1Mode, kLfoFree}, {pL1Rate, 0.6f}, {pL1Shape, kShTri},
        {pUnison, 2}, {pDetune, 25}, {pWidth, 80}, {pDrift, 25},
        {pAttack, 400}, {pSustain, 90}, {pRelease, 1200},
        {pCutoff, 4500}, {pReso, 5}, {pChorus, 45}, {pChMode, Chorus::kEnsemble}, {pReverb, 35},
        {pVoices, 12}, {pVelLevel, 40}, {pColMorph1, 50}, {pColMorph2, 50}, {pColFilter, 25}});

    // Stepped digital waves with a snappy morph envelope and sixteenth echoes.
    program("Pixel Pluck", {{pTable1, kTabDigital}, {pMorph1, 30}, {pTable2, kTabDigital}, {pMorph2, 70}, {pOct2, 1}, {pLevel2, 35},
        {pDecay, 300}, {pSustain, 0}, {pRelease, 200}, {pEnvMorph, 45}, {pMDecay, 200},
        {pFilterMode, kLP12}, {pCutoff, 7000}, {pPhase, 1}, {pVelLevel, 50},
        {pDelay, 35}, {pDlyTime, 2}, {pDlyFdbk, 40}, {pDlyTone, 70}, {pReverb, 15}, {pColMorph1, 60}});

    // Folded sine and saw over a sine sub; an eighth-note LFO locked to the song wobbles cutoff and morph.
    program("Wobble Deep", {{pTable1, kTabFold}, {pMorph1, 15}, {pMorph2, 50}, {pLevel2, 45}, {pFine2, 6},
        {pSub, 50}, {pFilterMode, kLP24}, {pCutoff, 300}, {pReso, 40}, {pDrive, 25}, {pKeyTrack, 30},
        {pL1Mode, kLfoSync}, {pL1Div, div8}, {pL1Shape, kShSine},
        {pSrc1, kSrcLfo1}, {pDst1, kDstCutoff}, {pAmt1, 45},
        {pSrc2, kSrcLfo1}, {pDst2, kDstMorph1}, {pAmt2, 40},
        {pPlayMode, kLegato}, {pGlide, 30}, {pPhase, 1}, {pVoices, 1},
        {pAttack, 1}, {pSustain, 100}, {pRelease, 100},
        {pColMorph1, 40}, {pColMorph2, 0}, {pColFilter, 40}, {pOutput, -4}});

    // A slow comb sweep over a hollow octave: phaser haze.
    program("Phaser Haze", {{pTable1, kTabComb}, {pMorph1, 30}, {pTable2, kTabHollow}, {pMorph2, 50}, {pOct2, -1}, {pLevel2, 45},
        {pL2Mode, kLfoFree}, {pL2Rate, 0.08f}, {pL2Shape, kShTri}, {pAmt2, 40},
        {pUnison, 3}, {pDetune, 30}, {pWidth, 90}, {pDrift, 30},
        {pAttack, 600}, {pSustain, 85}, {pRelease, 1800},
        {pFilterMode, kLP12}, {pCutoff, 7000}, {pChorus, 30}, {pReverb, 40}, {pDelay, 20}, {pDlyTime, 8},
        {pVoices, 10}, {pVelLevel, 40}, {pColMorph1, 60}, {pColMorph2, -30}, {pColFilter, 20}});

    // Phase-distortion funk: the filter envelope squashes the sine into a resonant bark.
    program("Inkjet Funk", {{pMorph1, 0}, {pWarpMode1, kWSquash}, {pWarp1, 15},
        {pSrc3, kSrcFiltEnv}, {pDst3, kDstWarp1}, {pAmt3, 65}, {pFDecay, 250}, {pFSustain, 0},
        {pMorph2, 50}, {pLevel2, 20}, {pSemi2, 12},
        {pFilterMode, kLP12}, {pDecay, 400}, {pSustain, 40}, {pRelease, 150},
        {pPhase, 1}, {pVelLevel, 60}, {pAmt4, 0}, {pDelay, 10}, {pDlyTime, 5},
        {pColMorph1, 40}, {pColFilter, 0}});

    // ---- 1.1 ---------------------------------------------------------------------------------------
    // Leads
    // A sync sweep on every note from the morph envelope, a saw an octave down, delayed vibrato.
    program("Sepia Sync Lead", {{pTable1, kTabSync}, {pMorph1, 15}, {pEnvMorph, 55}, {pMDecay, 350}, {pMSustain, 20},
        {pMorph2, 50}, {pOct2, -1}, {pLevel2, 40}, {pFine2, 4}, {pUnison, 2}, {pDetune, 15}, {pWidth, 40},
        {pPlayMode, kLegato}, {pGlide, 60}, {pCutoff, 6000}, {pReso, 15},
        {pL1Mode, kLfoRetrig}, {pL1Rate, 5.5f}, {pL1Fade, 500}, {pAmt1, 18},
        {pDelay, 20}, {pDlyTime, 7}, {pReverb, 15}, {pVoices, 1}, {pColMorph1, 40}, {pColFilter, 20}});
    // A mono square with a fast PWM shimmer from LFO 2.
    program("Squid Ink Square", {{pMorph1, 75}, {pTable2, kTabPWM}, {pMorph2, 30}, {pLevel2, 50}, {pFine2, -6},
        {pL2Mode, kLfoFree}, {pL2Rate, 4.0f}, {pL2Shape, kShSine}, {pAmt2, 12},
        {pPlayMode, kMono}, {pGlide, 40}, {pFilterMode, kLP12}, {pCutoff, 5000}, {pEnvAmt, 20}, {pFDecay, 400},
        {pDelay, 25}, {pDlyTime, 6}, {pReverb, 15}, {pVoices, 1}, {pColMorph1, -40}, {pColMorph2, 40}, {pColFilter, 20}});
    // A talking lead: the formant opens on each note; the mod wheel moves the mouth.
    program("Mantle Talk Lead", {{pTable1, kTabFormant}, {pMorph1, 25}, {pEnvMorph, 35}, {pMAttack, 80}, {pMDecay, 400},
        {pMSustain, 40}, {pSrc4, kSrcWheel}, {pDst4, kDstMorph1}, {pAmt4, 50}, {pSrc1, kSrcLfo1}, {pAmt1, 12},
        {pL1Mode, kLfoRetrig}, {pL1Rate, 5.2f}, {pL1Fade, 350}, {pUnison, 2}, {pDetune, 12}, {pWidth, 30},
        {pPlayMode, kLegato}, {pGlide, 80}, {pFilterMode, kLP12}, {pCutoff, 9000},
        {pDelay, 20}, {pDlyTime, 7}, {pReverb, 20}, {pVoices, 1}, {pColMorph1, 60}, {pColFilter, 0}});
    // A pure, breathy whistle of a lead: glassy sine, slow vibrato, a long slide.
    program("Glass Whistle", {{pTable1, kTabGlass}, {pMorph1, 35}, {pNoise, 6}, {pNoiseTone, 85},
        {pPlayMode, kLegato}, {pGlide, 130}, {pAttack, 40}, {pRelease, 400},
        {pL1Mode, kLfoRetrig}, {pL1Rate, 4.8f}, {pL1Fade, 700}, {pSrc1, kSrcLfo1}, {pAmt1, 14},
        {pDelay, 30}, {pDlyTime, 7}, {pDlyFdbk, 40}, {pReverb, 35}, {pRevSize, 70}, {pVoices, 1},
        {pColMorph1, 60}, {pColFilter, 0}});
    // Folded and driven; press the pad harder (aftertouch) to fold it further.
    program("Fold Screamer", {{pTable1, kTabFold}, {pMorph1, 35}, {pWarp1, 35}, {pSrc3, kSrcAT}, {pDst3, kDstMorph1}, {pAmt3, 40},
        {pUnison, 2}, {pDetune, 12}, {pWidth, 40}, {pFilterMode, kLP24}, {pCutoff, 3500}, {pReso, 35}, {pDrive, 40},
        {pEnvAmt, 30}, {pFDecay, 500}, {pFSustain, 30}, {pPlayMode, kMono}, {pGlide, 30},
        {pDelay, 15}, {pDlyTime, 5}, {pReverb, 15}, {pVoices, 1}, {pColMorph1, 50}, {pColFilter, 30}, {pOutput, -5}});

    // Pads
    // Two choirs singing through slowly wandering vowels.
    program("Abyssal Choir", {{pTable1, kTabVowel}, {pMorph1, 10}, {pTable2, kTabVowel}, {pMorph2, 60}, {pLevel1, 70}, {pLevel2, 60},
        {pFine2, -9}, {pSrc1, kSrcLfo1}, {pDst1, kDstMorph1}, {pAmt1, 40}, {pL1Mode, kLfoFree}, {pL1Rate, 0.07f}, {pL1Shape, kShSmooth},
        {pDst2, kDstMorph2}, {pAmt2, 40}, {pL2Rate, 0.11f}, {pL2Shape, kShSmooth},
        {pUnison, 3}, {pDetune, 22}, {pWidth, 90}, {pDrift, 30}, {pAttack, 1500}, {pSustain, 90}, {pRelease, 3000},
        {pFilterMode, kLP12}, {pCutoff, 5000}, {pChorus, 45}, {pChMode, Chorus::kEnsemble}, {pReverb, 55}, {pRevSize, 80},
        {pVoices, 10}, {pVelLevel, 30}, {pColMorph1, 60}, {pColMorph2, 30}, {pColFilter, 20}});
    // The classic: detuned saws, a slow filter swell, chorus.
    program("Warm Current Pad", {{pMorph1, 50}, {pMorph2, 50}, {pLevel2, 70}, {pFine2, 9}, {pUnison, 3}, {pDetune, 30}, {pWidth, 80},
        {pDrift, 25}, {pCutoff, 1800}, {pReso, 12}, {pEnvAmt, 25}, {pFAttack, 900}, {pFDecay, 2000}, {pFSustain, 50},
        {pAttack, 600}, {pSustain, 90}, {pRelease, 1500}, {pChorus, 40}, {pReverb, 35},
        {pVoices, 10}, {pVelLevel, 40}, {pColMorph1, 30}, {pColMorph2, 30}, {pColFilter, 45}});
    // Random spectra rolling in on a four-bar tide locked to the song.
    program("Spectral Tide", {{pTable1, kTabSpectra}, {pMorph1, 20}, {pTable2, kTabSpectra}, {pMorph2, 70}, {pLevel1, 70}, {pLevel2, 60},
        {pL2Mode, kLfoSync}, {pL2Div, div4Bars}, {pL2Shape, kShTri}, {pAmt2, 45},
        {pUnison, 2}, {pWidth, 90}, {pDrift, 30}, {pAttack, 800}, {pSustain, 90}, {pRelease, 2500},
        {pFilterMode, kLP12}, {pCutoff, 7000}, {pReverb, 50}, {pRevSize, 75}, {pDelay, 20}, {pDlyTime, 10},
        {pVoices, 10}, {pVelLevel, 40}, {pColMorph1, 50}, {pColMorph2, -50}, {pColFilter, 20}});
    // Drawbars under ice: a slow organ swell with an octave choir above.
    program("Frozen Organ Pad", {{pTable1, kTabOrgan}, {pMorph1, 30}, {pTable2, kTabOrgan}, {pMorph2, 80}, {pOct2, 1}, {pLevel2, 40},
        {pAmt2, 25}, {pDst2, kDstMorph2}, {pL2Rate, 0.09f}, {pL2Shape, kShSmooth},
        {pAttack, 1200}, {pSustain, 100}, {pRelease, 2500}, {pFilterMode, kLP12}, {pCutoff, 6000},
        {pChorus, 50}, {pChMode, Chorus::kEnsemble}, {pReverb, 60}, {pRevSize, 85}, {pDrift, 20},
        {pVoices, 10}, {pVelLevel, 20}, {pColMorph1, 50}, {pColMorph2, 20}, {pColFilter, 0}});
    // Hold a chord: the resonant sweep climbs over four seconds and stays open.
    program("Nautilus Sweep", {{pTable1, kTabReso}, {pMorph1, 5}, {pEnvMorph, 70}, {pMAttack, 4000}, {pMSustain, 100}, {pMRelease, 3000},
        {pMorph2, 50}, {pLevel2, 40}, {pFine2, 8}, {pUnison, 2}, {pWidth, 80},
        {pAttack, 400}, {pSustain, 90}, {pRelease, 2500}, {pReverb, 45}, {pDelay, 25}, {pDlyTime, 10},
        {pVoices, 10}, {pVelLevel, 40}, {pColMorph1, 40}, {pColFilter, 20}});

    // Plucks, keys and bells
    // Soft mallets: a bright knock that mellows at once.
    program("Cuttlebone Mallets", {{pTable1, kTabTine}, {pMorph1, 10}, {pEnvMorph, 50}, {pMDecay, 120}, {pMSustain, 0},
        {pTable2, kTabGlass}, {pMorph2, 0}, {pOct2, 2}, {pLevel2, 15}, {pFine2, 0},
        {pDecay, 600}, {pSustain, 0}, {pRelease, 500}, {pFilterMode, kLP12}, {pCutoff, 5000}, {pVelFilter, 40},
        {pPhase, 1}, {pVelLevel, 70}, {pReverb, 20}, {pColMorph1, 50}, {pColFilter, 20}});
    // A harp-like pluck: filter-plucked triangle-saw that follows the keyboard.
    program("Harp Strand", {{pMorph1, 30}, {pFilterMode, kLP12}, {pCutoff, 900}, {pEnvAmt, 55}, {pFDecay, 450}, {pFSustain, 0},
        {pKeyTrack, 70}, {pDecay, 1500}, {pSustain, 0}, {pRelease, 1200}, {pPhase, 1},
        {pChorus, 15}, {pReverb, 40}, {pDelay, 15}, {pDlyTime, 7}, {pVelLevel, 70}, {pColMorph1, 30}, {pColFilter, 40}, {pOutput, 0}});
    // FM electric piano: a sine phase-modulated by a sine an octave up; harder notes bark.
    program("Tine FM Piano", {{pMorph1, 0}, {pWarpMode1, kWFM}, {pWarp1, 22}, {pMorph2, 0}, {pOct2, 1}, {pFine2, 0}, {pLevel2, 0},
        {pSrc3, kSrcFiltEnv}, {pDst3, kDstWarp1}, {pAmt3, 30}, {pFDecay, 400}, {pFSustain, 0},
        {pSrc4, kSrcVel}, {pDst4, kDstWarp1}, {pAmt4, 35},
        {pDecay, 2500}, {pSustain, 10}, {pRelease, 500}, {pFilterMode, kLP12}, {pPhase, 1},
        {pChorus, 30}, {pChRate, 0.5f}, {pReverb, 20}, {pVelLevel, 75}, {pColMorph1, 0}, {pColMorph2, 0}, {pColFilter, 0}, {pOutput, 0}});
    // Clangorous plucked strings with an eighth-dotted echo.
    program("Coral Koto", {{pTable1, kTabMetal}, {pMorph1, 20}, {pEnvMorph, 40}, {pMDecay, 150}, {pMSustain, 0},
        {pDecay, 900}, {pSustain, 0}, {pRelease, 700}, {pFilterMode, kLP12}, {pCutoff, 7000}, {pVelFilter, 30},
        {pPhase, 1}, {pVelLevel, 70}, {pDelay, 30}, {pDlyTime, 7}, {pReverb, 30}, {pColMorph1, 50}, {pColFilter, 20}});
    // High glass drops into a long echo.
    program("Chime Drops", {{pTable1, kTabGlass}, {pMorph1, 85}, {pWarp1, 30}, {pOct1, 1},
        {pDecay, 1200}, {pSustain, 0}, {pRelease, 1000}, {pPhase, 1}, {pVelLevel, 70},
        {pDelay, 35}, {pDlyTime, 4}, {pDlyFdbk, 45}, {pReverb, 40}, {pRevSize, 70}, {pColMorph1, -50}, {pColFilter, 0}});
    // Brass stabs: short, punchy, opening with the morph envelope.
    program("Siphon Brass Stab", {{pTable1, kTabBrass}, {pMorph1, 30}, {pTable2, kTabBrass}, {pMorph2, 40}, {pFine2, 10}, {pLevel2, 60},
        {pEnvMorph, 50}, {pMAttack, 20}, {pMDecay, 300}, {pMSustain, 20}, {pUnison, 2}, {pDetune, 15}, {pWidth, 60},
        {pAttack, 8}, {pDecay, 400}, {pSustain, 30}, {pRelease, 200}, {pVelFilter, 30}, {pCutoff, 7000},
        {pReverb, 20}, {pVelLevel, 60}, {pColMorph1, 40}, {pColFilter, 30}});

    // Basses
    // Two detuned saws beating against each other, driven, over a sine sub.
    program("Reese Abyss", {{pMorph1, 50}, {pMorph2, 50}, {pLevel2, 75}, {pFine2, -16}, {pUnison, 2}, {pDetune, 20}, {pWidth, 50},
        {pSub, 40}, {pFilterMode, kLP24}, {pCutoff, 700}, {pReso, 10}, {pDrive, 35}, {pKeyTrack, 30},
        {pPlayMode, kLegato}, {pGlide, 50}, {pAttack, 2}, {pSustain, 100}, {pRelease, 150}, {pVoices, 1}, {pDrift, 15},
        {pColMorph1, 0}, {pColMorph2, 0}, {pColFilter, 50}, {pOutput, -5}});
    // A sync sweep on every note, with a square sub.
    program("Sync Snap Bass", {{pTable1, kTabSync}, {pMorph1, 0}, {pEnvMorph, 60}, {pMDecay, 180}, {pMSustain, 0},
        {pSub, 30}, {pSubType, 3}, {pFilterMode, kLP24}, {pCutoff, 900}, {pEnvAmt, 40}, {pFDecay, 200}, {pFSustain, 20},
        {pPlayMode, kLegato}, {pGlide, 25}, {pAttack, 1}, {pSustain, 85}, {pRelease, 100}, {pPhase, 1}, {pVoices, 1},
        {pColMorph1, 40}, {pColFilter, 30}, {pOutput, 0}});
    // A sine thump: phase modulation that falls away fast, a sine sub under it.
    program("FM Thump Bass", {{pMorph1, 0}, {pWarpMode1, kWFM}, {pWarp1, 15}, {pMorph2, 0}, {pFine2, 0},
        {pSrc3, kSrcFiltEnv}, {pDst3, kDstWarp1}, {pAmt3, 50}, {pFDecay, 150}, {pFSustain, 0},
        {pSub, 40}, {pFilterMode, kLP12}, {pDecay, 600}, {pSustain, 70}, {pRelease, 100},
        {pPlayMode, kMono}, {pPhase, 1}, {pVoices, 1}, {pColMorph1, 30}, {pColFilter, 0}, {pOutput, -1}});
    // A squelchy resonant saw for acid lines; slide by overlapping notes.
    program("Acid Morph", {{pMorph1, 50}, {pFilterMode, kLP24}, {pCutoff, 400}, {pReso, 70}, {pDrive, 45}, {pEnvAmt, 55},
        {pFDecay, 220}, {pFSustain, 10}, {pVelFilter, 40}, {pKeyTrack, 30},
        {pPlayMode, kLegato}, {pGlide, 70}, {pAttack, 1}, {pSustain, 100}, {pRelease, 80}, {pPhase, 1}, {pVoices, 1},
        {pDelay, 15}, {pDlyTime, 6}, {pColMorph1, 25}, {pColFilter, 50}, {pOutput, -5}});
    // Deep and round: a hollow wave two octaves over a sub-sub sine.
    program("Benthic Sub", {{pTable1, kTabHollow}, {pMorph1, 0}, {pSub, 55}, {pSubType, 1}, {pFilterMode, kLP12}, {pCutoff, 600},
        {pKeyTrack, 40}, {pDecay, 400}, {pSustain, 60}, {pRelease, 120}, {pPlayMode, kMono}, {pPhase, 1}, {pVoices, 1},
        {pColMorph1, 50}, {pColFilter, 40}, {pOutput, -1}});

    // Textures and effects
    // Sparkles on the sixteenths (locked to the song), drifting across the stereo field.
    program("Bioluminescence", {{pTable1, kTabGlass}, {pMorph1, 40}, {pTable2, kTabMetal}, {pMorph2, 30}, {pOct2, 1}, {pLevel2, 45},
        {pL1Mode, kLfoSync}, {pL1Div, div16}, {pL1Shape, kShSH}, {pSrc1, kSrcLfo1}, {pDst1, kDstMorph2}, {pAmt1, 50},
        {pL2Rate, 0.1f}, {pL2Shape, kShSmooth}, {pDst2, kDstPan}, {pAmt2, 40},
        {pAttack, 300}, {pSustain, 90}, {pRelease, 3000}, {pDelay, 40}, {pDlyTime, 7}, {pDlyFdbk, 55},
        {pReverb, 60}, {pRevSize, 85}, {pVoices, 10}, {pVelLevel, 40}, {pColMorph1, 50}, {pColMorph2, 50}, {pColFilter, 0}});
    // A sonar ping: a sine that falls into pitch, echoing out into the deep.
    program("Sonar Ping", {{pMorph1, 0}, {pSrc3, kSrcModEnv}, {pDst3, kDstPitch}, {pAmt3, 30}, {pMDecay, 80}, {pMSustain, 0},
        {pDecay, 2000}, {pSustain, 0}, {pRelease, 1500}, {pPhase, 1}, {pFilterMode, kLP12},
        {pDelay, 50}, {pDlyTime, 8}, {pDlyFdbk, 60}, {pDlyTone, 30}, {pReverb, 60}, {pRevSize, 80}, {pVelLevel, 60},
        {pColMorph1, 40}, {pColFilter, 0}});
    // Chords that pulse on the quarter-note triplets of the song.
    program("Ripple Chords", {{pMorph1, 50}, {pMorph2, 60}, {pLevel2, 50}, {pUnison, 2}, {pDetune, 20}, {pWidth, 70},
        {pFilterMode, kLP24}, {pCutoff, 900}, {pReso, 30}, {pL1Mode, kLfoSync}, {pL1Div, 8}, {pL1Shape, kShSine},
        {pSrc1, kSrcLfo1}, {pDst1, kDstCutoff}, {pAmt1, 35}, {pAttack, 5}, {pSustain, 100}, {pRelease, 300},
        {pChorus, 20}, {pReverb, 25}, {pVoices, 10}, {pVelLevel, 50}, {pColMorph1, 30}, {pColFilter, 40}});
    // A glitching storm: stepped digital waves, sample-and-hold morph and a falling filter on the grid.
    program("Ink Storm", {{pTable1, kTabDigital}, {pMorph1, 50}, {pL1Mode, kLfoSync}, {pL1Div, div16}, {pL1Shape, kShSH},
        {pSrc1, kSrcLfo1}, {pDst1, kDstMorph1}, {pAmt1, 60},
        {pL2Mode, kLfoSync}, {pL2Div, div8}, {pL2Shape, kShSawDown}, {pDst2, kDstCutoff}, {pAmt2, 30},
        {pNoise, 25}, {pNoiseTone, 50}, {pFilterMode, kLP12}, {pCutoff, 3000}, {pReso, 40},
        {pAttack, 10}, {pSustain, 100}, {pRelease, 400}, {pDelay, 30}, {pDlyTime, 5}, {pReverb, 20},
        {pVoices, 10}, {pColMorph1, 40}, {pColFilter, 30}});

    (void)divBar1;
}
