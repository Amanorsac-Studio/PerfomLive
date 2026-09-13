// ============================================================================
//  stemplaytest.cpp — standalone console test for PLAN_ARRANGEMENT_VIEW.md
//  Milestone 2 (linear playthrough of one long stem).
//
//  Pure C++, no JUCE, no CMake, no audio device -- same convention as
//  dsptest.cpp/decktest.cpp/switchtest.cpp/stemlentest.cpp.
//
//  IMPORTANT CONTEXT, confirmed by direct audit of Deck.h and Session.h
//  before writing a single line here: M2's engine requirements are ALREADY
//  implemented, by code that predates this plan entirely --
//    - Deck::render()/renderPerTab() (Deck.h, tagged "Milestone 5, M5-T2" in
//      this codebase's own earlier numbering) already play a stem-mode
//      layer from playhead 0 through exactly layer.numFrames() with NO
//      wraparound, silencing once past it -- a real per-sample playhead,
//      not a bar-4 region.
//    - Deck::stemLength()/stemFinished() (Deck.h, same era) already report
//      the deck's true natural end (longest LOADED layer's numFrames()) and
//      whether the shared playhead has reached it.
//    - Session::render() (Session.h, tagged "Milestone 5, M5-T3/M5-T4")
//      already clamps each chunk so a stem-mode deck's true end is detected
//      at the EXACT sample regardless of caller block size, and already
//      triggers all three StemEndBehavior cases (next/loop/nextPlay) right
//      there -- switchtest.cpp already proves this exhaustively (see its own
//      "M5-T3"/"M5-T4" sections).
//    - None of this reads Layer::regionLength in stem mode at all -- so
//      there was never a bar-4 truncation in PLAYBACK to begin with. The
//      real gap M1 closed was purely at IMPORT time (no 32-bar cap, no
//      stemBarLength bookkeeping); the playback engine underneath was
//      already correct.
//
//  So this file adds ZERO new production code -- Deck.h and Session.h are
//  UNCHANGED by this milestone. What's new here is proof AT THE SCALE THIS
//  FEATURE ACTUALLY USES: every existing stem test (decktest.cpp,
//  switchtest.cpp) uses small synthetic lengths (100-1000 samples) chosen
//  for test-authoring convenience, never tied to a real bar count. This file
//  builds layers sized via the same bars-to-samples arithmetic
//  ezdeck::resolveStemBarLength (Deck.h, M1) uses, at 16 and 32 bars
//  specifically, and -- the key adversarial check -- plants a STALE 4-bar
//  regionLength on the very same layer (the value loop mode's fitFourBars
//  would have produced) to prove playback and end-behavior triggering both
//  ignore it completely and fire at the TRUE end instead.
//
//  Build & run (no CMake needed -- zero JUCE dependency):
//    cl /nologo /EHsc /std:c++17 /Fe:stemplaytest.exe stemplaytest.cpp
//    ./stemplaytest.exe
// ============================================================================
#include "Session.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int gPass = 0, gTotal = 0;

#define CHECK(cond, desc) do {                                    \
    ++gTotal;                                                     \
    if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); }    \
    else      {          std::printf ("  [FAIL] %s\n", desc); }    \
} while (0)

static std::vector<float> makeRamp (int n)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) i;
    return v;
}

// Same beatsAt()-style arithmetic as EzDSP.h's fitFourBars and Deck.h's
// resolveStemBarLength, in raw samples at an arbitrary abstract sample rate
// (matching switchtest.cpp's own convention of small, convenient "sample
// rates" -- these are unit tests of sample-count arithmetic, not audio
// fidelity).
static int barsToSamples (int bars, double bpm, double sampleRate)
{
    const double secondsPerBar = (60.0 / bpm) * 4.0;
    return (int) std::llround ((double) bars * secondsPerBar * sampleRate);
}

int main()
{
    using namespace ezdeck;

    std::printf ("Deck.h/Session.h stem linear-playthrough tests (PLAN_ARRANGEMENT_VIEW.md M2)\n");

    const double bpm = 120.0;
    const double sr  = 1000.0;   // abstract; only the sample-count math matters here
    const int fourBarEnd = barsToSamples (4, bpm, sr);

    // ---- single layer, 16 bars: reaches its TRUE end, not bar 4 -----------
    {
        const int bars = 16;
        const int trueEnd = barsToSamples (bars, bpm, sr);

        Deck d;
        d.mode = DeckMode::stem;
        d.layers[0].left = makeRamp (trueEnd);
        d.layers[0].loaded = true;
        // a stale 4-bar region, exactly like loop mode's fitFourBars would
        // have set -- proves stem playback ignores it entirely
        d.layers[0].regionLength = fourBarEnd;

        std::vector<float> outL ((size_t) (trueEnd - 1)), outR ((size_t) (trueEnd - 1));
        d.render (outL.data(), outR.data(), trueEnd - 1, 1.0f);

        CHECK (d.playheadPosition() > (double) fourBarEnd,
               "16-bar stem: playhead has advanced well past the stale 4-bar region without stopping or wrapping");
        CHECK (! d.stemFinished(),
               "16-bar stem: not finished one sample before its TRUE 16-bar end");

        float lastL, lastR;
        d.render (&lastL, &lastR, 1, 1.0f);

        CHECK (d.stemFinished(),
               "16-bar stem: stemFinished() becomes true exactly at the TRUE 16-bar end (not bar 4)");
        CHECK (d.playheadPosition() == (double) trueEnd,
               "16-bar stem: playhead lands exactly on the true end sample count, not truncated");
    }

    // ---- single layer, 32 bars: reaches its TRUE end, not bar 4 -----------
    {
        const int bars = 32;
        const int trueEnd = barsToSamples (bars, bpm, sr);

        Deck d;
        d.mode = DeckMode::stem;
        d.layers[0].left = makeRamp (trueEnd);
        d.layers[0].loaded = true;
        d.layers[0].regionLength = fourBarEnd;   // same stale-region adversarial check

        std::vector<float> outL ((size_t) (trueEnd - 1)), outR ((size_t) (trueEnd - 1));
        d.render (outL.data(), outR.data(), trueEnd - 1, 1.0f);

        CHECK (d.playheadPosition() > (double) fourBarEnd,
               "32-bar stem: playhead has advanced well past the stale 4-bar region without stopping or wrapping");
        CHECK (! d.stemFinished(),
               "32-bar stem: not finished one sample before its TRUE 32-bar end");

        float lastL, lastR;
        d.render (&lastL, &lastR, 1, 1.0f);

        CHECK (d.stemFinished(),
               "32-bar stem: stemFinished() becomes true exactly at the TRUE 32-bar end (not bar 4)");
        CHECK (d.playheadPosition() == (double) trueEnd,
               "32-bar stem: playhead lands exactly on the true end sample count, not truncated");
    }

    // ---- end-behaviour fires at the TRUE end: loop --------------------------
    {
        const int bars = 16;
        const int trueEnd = barsToSamples (bars, bpm, sr);
        Tempo t { bpm, 4 };

        Session<1> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].mode = DeckMode::stem;
        s.decks[0].stemEndBehavior = StemEndBehavior::loop;
        s.decks[0].layers[0].left = makeRamp (trueEnd);
        s.decks[0].layers[0].loaded = true;
        s.decks[0].layers[0].regionLength = fourBarEnd;   // stale 4-bar region present

        std::vector<float> L1 ((size_t) (trueEnd - 1)), R1 ((size_t) (trueEnd - 1));
        s.render (L1.data(), R1.data(), trueEnd - 1, 1.0f);
        CHECK (s.decks[0].playheadPosition() > (double) fourBarEnd && ! s.decks[0].stemFinished(),
               "loop end-behavior: still playing (not reset, not finished) one sample before the TRUE 16-bar end, despite a stale 4-bar region");

        float oneL, oneR;
        s.render (&oneL, &oneR, 1, 1.0f);
        CHECK (s.decks[0].playheadPosition() == 0.0,
               "loop end-behavior: resets EXACTLY at the TRUE 16-bar end (playhead back to 0 the same sample it finishes), not at the stale 4-bar region");
    }

    // ---- end-behaviour fires at the TRUE end: nextPlay ---------------------
    {
        const int bars = 16;
        const int trueEnd = barsToSamples (bars, bpm, sr);
        Tempo t { bpm, 4 };

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].mode = DeckMode::stem;   // stemEndBehavior defaults to nextPlay
        s.decks[0].layers[0].left = makeRamp (trueEnd);
        s.decks[0].layers[0].loaded = true;
        s.decks[0].layers[0].regionLength = fourBarEnd;   // stale 4-bar region present
        s.decks[1].layers[0].left = makeRamp (trueEnd);
        s.decks[1].layers[0].loaded = true;

        std::vector<float> L1 ((size_t) (trueEnd - 1)), R1 ((size_t) (trueEnd - 1));
        s.render (L1.data(), R1.data(), trueEnd - 1, 1.0f);
        CHECK (s.activeDeck() == 0 && s.queuedDeck() == -1,
               "nextPlay end-behavior: nothing queued yet one sample before the TRUE 16-bar end, despite a stale 4-bar region");

        float oneL, oneR;
        s.render (&oneL, &oneR, 1, 1.0f);
        CHECK (s.activeDeck() == 0,
               "nextPlay end-behavior: the deck stays active immediately after its stem finishes -- the actual switch is still bar-quantized");
        CHECK (s.queuedDeck() == 1,
               "nextPlay end-behavior: auto-queues the next deck EXACTLY at the TRUE 16-bar end (not the stale 4-bar region)");
    }

    // ---- loop mode is completely unaffected: same-sized material, real
    //      4-bar region, still loops there (never reaches the 16-bar end) ---
    {
        const int bars = 16;
        const int trueEnd = barsToSamples (bars, bpm, sr);

        Deck d;   // defaults to DeckMode::loop
        d.layers[0].left = makeRamp (trueEnd);   // same 16-bar-sized source material
        d.layers[0].loaded = true;
        d.layers[0].regionLength = fourBarEnd;   // loop mode's own tempo-derived 4-bar region, exactly as loadLayer() sets today

        std::vector<float> outL ((size_t) fourBarEnd), outR ((size_t) fourBarEnd);
        d.render (outL.data(), outR.data(), fourBarEnd, 1.0f);

        CHECK (d.phaseOf (0) == 0.0,
               "loop mode: after exactly one 4-bar region's worth of samples, phase wraps back to 0 -- unaffected by M2 (still loops at its region, never reaches the 16-bar buffer's true end)");
        CHECK (! d.stemFinished(),
               "loop mode: stemFinished() is always false for a loop-mode deck, regardless of how long the underlying buffer actually is");
    }

    std::printf ("\n%d/%d tests passed\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
