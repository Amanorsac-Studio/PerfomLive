// ============================================================================
//  stemlocktest.cpp — standalone console test for PLAN_ARRANGEMENT_VIEW.md
//  Milestone 3 (four long stem layers, sample-locked, played through
//  together) AND Milestone 4 (live mute/unmute per layer during stem
//  playthrough, click-free) -- see each milestone's own section below for
//  what was audited-as-already-working (M3: everything) vs. genuinely new
//  production code (M4: the mute ramp; see Deck.h's Layer::muteGain comment).
//
//  Pure C++, no JUCE, no CMake, no audio device -- same convention as
//  dsptest.cpp/decktest.cpp/switchtest.cpp/stemlentest.cpp/stemplaytest.cpp.
//
//  AUDIT FINDING (per M3's own explicit instruction: audit before writing
//  production code) -- confirmed by direct read of Deck::render() and
//  Deck::renderPerTab() (Deck.h) before writing a single test here:
//
//    - Both methods use ONE shared `playhead` (a single private double).
//      Their outer loop is `for (i in 0..numSamples) { for (each layer) {
//      ...reads playhead... } playhead += rateRatio; }` -- every layer's
//      inner loop body runs BEFORE playhead is incremented for that sample,
//      so all four layers ALWAYS read the exact same playhead value at a
//      given output sample index. This isn't just tested to be sample-locked
//      -- it's structurally impossible for the layers to drift apart, since
//      there is no per-layer position state to drift; there is only the one
//      shared double every layer reads from within the same iteration. This
//      predates PLAN_ARRANGEMENT_VIEW.md entirely (Deck.h's own "Milestone 5,
//      M5-T2" tag) and decktest.cpp already proves it via renderPerTab()'s
//      cross-check against render()'s pre-summed output.
//
//    - The "different lengths" design question M3 asks about is ALREADY
//      decided and already tested by decktest.cpp's own "M5-T2: mismatched-
//      length layers in stem mode stop independently" section (at small
//      synthetic sizes, 50 vs 150 samples): a shorter layer silences itself
//      once playhead passes its own numFrames(), while a longer layer in the
//      same deck keeps playing; the DECK's own natural end
//      (stemLength()/stemFinished()) is governed by the LONGEST loaded
//      layer, not the shortest. This is the answer, already built: the deck
//      plays until its longest layer finishes; shorter layers simply go
//      quiet earlier and the true end is nowhere near being "chopped" to the
//      shortest one.
//
//  So, exactly as with M2: this file adds ZERO new production code --
//  Deck.h is UNCHANGED by this milestone. What's new here is proof AT THE
//  SCALE THIS FEATURE ACTUALLY USES and ACROSS THE FULL LENGTH, not just the
//  start: four IDENTICAL-content layers at a real 32-bar length, checked for
//  sample lock at five points spanning the whole playthrough (0%, 25%, 50%,
//  75%, the very last sample) -- proving lock is held throughout, not just
//  at the beginning where drift would be invisible over a short span. Plus a
//  16-bar/32-bar mismatched-length case at real bar-scale, re-proving
//  decktest.cpp's existing small-scale finding at the numbers this feature
//  will actually see.
//
//  Build & run (no CMake needed -- zero JUCE dependency):
//    cl /nologo /EHsc /std:c++17 /Fe:stemlocktest.exe stemlocktest.cpp
//    ./stemlocktest.exe
// ============================================================================
#include "Deck.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

static int gPass = 0, gTotal = 0;

#define CHECK(cond, desc) do {                                    \
    ++gTotal;                                                     \
    if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); }    \
    else      {          std::printf ("  [FAIL] %s\n", desc); }    \
} while (0)

static bool nearlyEqual (double a, double b, double eps = 1e-9)
{
    return std::fabs (a - b) <= eps;
}

static std::vector<float> makeRamp (int n)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) i;
    return v;
}

// Same beatsAt()-style arithmetic as EzDSP.h's fitFourBars, Deck.h's
// resolveStemBarLength, and stemplaytest.cpp's own helper.
static int barsToSamples (int bars, double bpm, double sampleRate)
{
    const double secondsPerBar = (60.0 / bpm) * 4.0;
    return (int) std::llround ((double) bars * secondsPerBar * sampleRate);
}

int main()
{
    using namespace ezdeck;

    std::printf ("Deck.h four-layer stem sample-lock tests (PLAN_ARRANGEMENT_VIEW.md M3)\n");

    const double bpm = 120.0;
    const double sr  = 1000.0;   // abstract; only the sample-count math matters here

    // ---- all 4 layers, same 32-bar length, sample-locked across the FULL
    //      playthrough -- checked at 5 points, not just the start ----------
    {
        const int bars = 32;
        const int trueEnd = barsToSamples (bars, bpm, sr);

        Deck d;
        d.mode = DeckMode::stem;
        for (int l = 0; l < kNumLayers; ++l)
        {
            // identical ramp per layer: value at index i is exactly i, so
            // reading back a layer's own output at output-sample k reveals
            // exactly which source position it read -- if every layer's
            // value at k equals k, every layer read the SAME position.
            d.layers[(size_t) l].left = makeRamp (trueEnd);
            d.layers[(size_t) l].loaded = true;
        }

        std::array<std::vector<float>, (size_t) kNumLayers> outL, outR;
        std::array<float*, kNumLayers> outLPtr {}, outRPtr {};
        for (int l = 0; l < kNumLayers; ++l)
        {
            outL[(size_t) l].assign ((size_t) trueEnd, 0.0f);
            outR[(size_t) l].assign ((size_t) trueEnd, 0.0f);
            outLPtr[(size_t) l] = outL[(size_t) l].data();
            outRPtr[(size_t) l] = outR[(size_t) l].data();
        }

        // block-size independence while we're at it -- an arbitrary,
        // non-divisor chunk size, matching decktest.cpp's own convention.
        int done = 0;
        const int chunkSize = 777;
        while (done < trueEnd)
        {
            const int chunk = std::min (chunkSize, trueEnd - done);
            std::array<float*, kNumLayers> chunkL {}, chunkR {};
            for (int l = 0; l < kNumLayers; ++l)
            {
                chunkL[(size_t) l] = outLPtr[(size_t) l] + done;
                chunkR[(size_t) l] = outRPtr[(size_t) l] + done;
            }
            d.renderPerTab (chunkL, chunkR, chunk);
            done += chunk;
        }

        const int checkpoints[] = { 0, trueEnd / 4, trueEnd / 2, (trueEnd * 3) / 4, trueEnd - 1 };
        const char* checkpointNames[] = { "start (0%)", "25%", "50%", "75%", "the very last sample" };

        for (int cpi = 0; cpi < 5; ++cpi)
        {
            const int cp = checkpoints[cpi];
            bool allLocked = true;
            for (int l = 0; l < kNumLayers; ++l)
                if (! nearlyEqual ((double) outL[(size_t) l][(size_t) cp], (double) cp, 1e-3))
                    allLocked = false;

            char desc[160];
            std::snprintf (desc, sizeof (desc),
                           "all 4 layers of a 32-bar stem deck read the IDENTICAL source position at the %s checkpoint",
                           checkpointNames[cpi]);
            CHECK (allLocked, desc);
        }
    }

    // ---- mismatched lengths: a 16-bar layer + a 32-bar layer in one deck --
    {
        const int shortBars = 16, longBars = 32;
        const int shortLen = barsToSamples (shortBars, bpm, sr);
        const int longLen  = barsToSamples (longBars, bpm, sr);

        Deck d;
        d.mode = DeckMode::stem;
        auto shortRamp = makeRamp (shortLen);
        auto longRamp  = makeRamp (longLen);
        for (auto& v : longRamp) v *= 2.0f;   // distinguishable content

        d.layers[0].left = shortRamp; d.layers[0].loaded = true;
        d.layers[1].left = longRamp;  d.layers[1].loaded = true;

        CHECK (d.stemLength() == longLen,
               "mismatched layers: deck's stemLength() is the LONGER (32-bar) layer's length, not the shorter (16-bar) one -- the deck's natural end is governed by its longest loaded layer");

        Deck longOnly;
        longOnly.mode = DeckMode::stem;
        longOnly.layers[0].left = longRamp; longOnly.layers[0].loaded = true;

        std::vector<float> L ((size_t) (longLen - 1)), R ((size_t) (longLen - 1));
        std::vector<float> refL ((size_t) (longLen - 1)), refR ((size_t) (longLen - 1));
        d.render (L.data(), R.data(), longLen - 1, 1.0f);
        longOnly.render (refL.data(), refR.data(), longLen - 1, 1.0f);

        bool matchesAfterShortEnds = true;
        for (int i = shortLen; i < longLen - 1; ++i)
            if (! nearlyEqual ((double) L[(size_t) i], (double) refL[(size_t) i], 1e-3))
                { matchesAfterShortEnds = false; break; }

        CHECK (matchesAfterShortEnds,
               "mismatched layers: after the 16-bar layer's own true end, the deck's output exactly matches the 32-bar layer alone -- the shorter layer silenced itself, the longer one plays on unaffected");
        CHECK (! d.stemFinished(),
               "mismatched layers: not finished one sample before the LONGER (32-bar) layer's true end, even though the shorter (16-bar) layer finished long ago");

        float lastL, lastR;
        d.render (&lastL, &lastR, 1, 1.0f);
        CHECK (d.stemFinished(),
               "mismatched layers: stemFinished() becomes true exactly at the LONGER (32-bar) layer's true end, not the shorter (16-bar) one's");
    }

    // ========================================================================
    //  PLAN_ARRANGEMENT_VIEW.md M4 (live mute/unmute per layer during stem
    //  playthrough). UNLIKE M2/M3, this milestone genuinely required new
    //  production code -- see Deck.h's own Layer::muteFadeSamples/muteGain
    //  comments for why: toggling `enabled` was already a hard, instant cut
    //  in both render() and renderPerTab() (and Mixer.h's own mute path is
    //  the same, checked before writing anything), with no click-avoidance
    //  anywhere in this codebase to reuse as-is. The fix adds a persisted
    //  per-layer ramp (muteGain) that degenerates to the exact old hard-cut
    //  when muteFadeSamples == 0 (every caller before this milestone), so
    //  loop mode and every existing stem test above are provably unaffected
    //  -- confirmed by the full regression this file's own build script runs
    //  (decktest/switchtest/stemlentest/stemplaytest all still pass
    //  unchanged).
    // ========================================================================

    auto renderChunk = [] (Deck& dk, std::array<float*, kNumLayers>& lptrs, std::array<float*, kNumLayers>& rptrs, int offset, int count)
    {
        std::array<float*, kNumLayers> chunkL {}, chunkR {};
        for (int l = 0; l < kNumLayers; ++l)
        {
            chunkL[(size_t) l] = lptrs[(size_t) l] + offset;
            chunkR[(size_t) l] = rptrs[(size_t) l] + offset;
        }
        dk.renderPerTab (chunkL, chunkR, count);
    };

    // ---- M4: mute ramps down click-free, others stay locked, unmute ramps
    //      back up reading the CURRENT (post-hold) playhead -- not the
    //      position where it was muted -----------------------------------
    {
        const int bars = 16;
        const int trueEnd = barsToSamples (bars, bpm, sr);
        const int muteFade = 100;   // short fade, in samples -- same "0 = no-op" convention as fadeInSamples/fadeOutSamples

        const int preMuteSamples   = 1000;   // play a while before muting
        const int mutedHoldSamples = 500;    // stay muted a while (proves it's not just a one-sample blip)
        const int postUnmuteSamples = 500;   // play a while after unmuting, fully back
        const int total = preMuteSamples + muteFade + mutedHoldSamples + muteFade + postUnmuteSamples;

        // Reference deck: IDENTICAL setup, layer 0 NEVER muted -- the ground
        // truth for what layer 0's content should be at any absolute output
        // sample index, since stem mode's pos=playhead is deck-shared and
        // completely unaffected by any one layer's mute state.
        Deck ref;
        ref.mode = DeckMode::stem;
        for (int l = 0; l < kNumLayers; ++l) { ref.layers[(size_t) l].left = makeRamp (trueEnd); ref.layers[(size_t) l].loaded = true; }

        Deck d;
        d.mode = DeckMode::stem;
        for (int l = 0; l < kNumLayers; ++l) { d.layers[(size_t) l].left = makeRamp (trueEnd); d.layers[(size_t) l].loaded = true; }
        d.layers[0].muteFadeSamples = muteFade;

        std::array<std::vector<float>, kNumLayers> outL, outR, refL, refR;
        std::array<float*, kNumLayers> outLPtr {}, outRPtr {}, refLPtr {}, refRPtr {};
        for (int l = 0; l < kNumLayers; ++l)
        {
            outL[(size_t) l].assign ((size_t) total, 0.0f); outR[(size_t) l].assign ((size_t) total, 0.0f);
            refL[(size_t) l].assign ((size_t) total, 0.0f); refR[(size_t) l].assign ((size_t) total, 0.0f);
            outLPtr[(size_t) l] = outL[(size_t) l].data(); outRPtr[(size_t) l] = outR[(size_t) l].data();
            refLPtr[(size_t) l] = refL[(size_t) l].data(); refRPtr[(size_t) l] = refR[(size_t) l].data();
        }

        int done = 0;

        // before muting: both decks should behave identically
        renderChunk (d, outLPtr, outRPtr, done, preMuteSamples);
        renderChunk (ref, refLPtr, refRPtr, done, preMuteSamples);
        done += preMuteSamples;

        bool identicalBeforeMute = true;
        for (int i = 0; i < done && identicalBeforeMute; ++i)
            for (int l = 0; l < kNumLayers; ++l)
                if (! nearlyEqual ((double) outL[(size_t) l][(size_t) i], (double) refL[(size_t) l][(size_t) i], 1e-4))
                    { identicalBeforeMute = false; break; }
        CHECK (identicalBeforeMute, "before muting: test deck matches the never-muted reference exactly (sanity check)");

        // mute layer 0
        d.layers[0].enabled = false;

        renderChunk (d, outLPtr, outRPtr, done, muteFade);
        renderChunk (ref, refLPtr, refRPtr, done, muteFade);
        const int fadeDownStart = done;
        done += muteFade;

        bool fadesDownLinearly = true;
        for (int k = 0; k < muteFade; ++k)
        {
            const int i = fadeDownStart + k;
            const float expectedGain = 1.0f - (float) (k + 1) / (float) muteFade;
            const float expectedValue = (float) i * expectedGain;   // ramp content at absolute index i, times the ramp's own gain
            if (! nearlyEqual ((double) outL[0][(size_t) i], (double) expectedValue, 1e-2)) { fadesDownLinearly = false; break; }
        }
        CHECK (fadesDownLinearly, "muting layer 0: its output ramps DOWN linearly to silence over exactly muteFadeSamples -- not a hard, instant cut");

        bool othersUnchangedDuringFadeDown = true;
        for (int i = fadeDownStart; i < done && othersUnchangedDuringFadeDown; ++i)
            for (int l = 1; l < kNumLayers; ++l)
                if (! nearlyEqual ((double) outL[(size_t) l][(size_t) i], (double) refL[(size_t) l][(size_t) i], 1e-4))
                    { othersUnchangedDuringFadeDown = false; break; }
        CHECK (othersUnchangedDuringFadeDown, "muting layer 0: layers 1-3 stay bit-identical to the reference throughout layer 0's fade-down -- unperturbed, still sample-locked");

        // hold muted for a while
        renderChunk (d, outLPtr, outRPtr, done, mutedHoldSamples);
        renderChunk (ref, refLPtr, refRPtr, done, mutedHoldSamples);
        const int holdStart = done;
        done += mutedHoldSamples;

        bool staysFullySilent = true;
        for (int i = holdStart; i < done; ++i)
            if (outL[0][(size_t) i] != 0.0f) { staysFullySilent = false; break; }
        CHECK (staysFullySilent, "muted layer 0 stays fully silent (not a partial level or a blip) for the entire hold period");

        bool othersUnchangedDuringHold = true;
        for (int i = holdStart; i < done && othersUnchangedDuringHold; ++i)
            for (int l = 1; l < kNumLayers; ++l)
                if (! nearlyEqual ((double) outL[(size_t) l][(size_t) i], (double) refL[(size_t) l][(size_t) i], 1e-4))
                    { othersUnchangedDuringHold = false; break; }
        CHECK (othersUnchangedDuringHold, "layers 1-3 remain bit-identical to the reference during layer 0's mute hold -- still sample-locked, unmoved by the mute");

        // unmute
        d.layers[0].enabled = true;

        renderChunk (d, outLPtr, outRPtr, done, muteFade);
        renderChunk (ref, refLPtr, refRPtr, done, muteFade);
        const int fadeUpStart = done;
        done += muteFade;

        bool fadesUpLinearly = true;
        for (int k = 0; k < muteFade; ++k)
        {
            const int i = fadeUpStart + k;
            const float expectedGain = (float) (k + 1) / (float) muteFade;
            // the shared playhead kept advancing the ENTIRE time layer 0 was
            // muted -- this uses the absolute index i (not a paused/frozen
            // index), which is exactly the "resumes in time" proof.
            const float expectedValue = (float) i * expectedGain;
            if (! nearlyEqual ((double) outL[0][(size_t) i], (double) expectedValue, 1e-2)) { fadesUpLinearly = false; break; }
        }
        CHECK (fadesUpLinearly, "unmuting layer 0: its output ramps back UP linearly over exactly muteFadeSamples, reading the CURRENT (post-hold) shared-playhead position");

        // once fully back, layer 0 must be bit-identical to the never-muted
        // reference AT THE SAME ABSOLUTE SAMPLE INDEX -- the definitive proof
        // that it resumed in time, not from a frozen/paused position (which
        // would read a LOWER, stale index instead).
        renderChunk (d, outLPtr, outRPtr, done, postUnmuteSamples);
        renderChunk (ref, refLPtr, refRPtr, done, postUnmuteSamples);
        const int steadyStart = done;
        done += postUnmuteSamples;

        bool resumedInTime = true;
        for (int i = steadyStart; i < done; ++i)
            if (! nearlyEqual ((double) outL[0][(size_t) i], (double) refL[0][(size_t) i], 1e-4)) { resumedInTime = false; break; }
        CHECK (resumedInTime, "once fully unmuted, layer 0 is bit-identical to the never-muted reference at the SAME absolute sample index -- it resumed IN TIME (tracking the shared playhead throughout its mute), not from where it paused");

        bool othersStillLockedAtEnd = true;
        for (int i = steadyStart; i < done && othersStillLockedAtEnd; ++i)
            for (int l = 1; l < kNumLayers; ++l)
                if (! nearlyEqual ((double) outL[(size_t) l][(size_t) i], (double) refL[(size_t) l][(size_t) i], 1e-4))
                    { othersStillLockedAtEnd = false; break; }
        CHECK (othersStillLockedAtEnd, "layers 1-3 remain bit-identical to the reference through the ENTIRE mute/unmute cycle -- completely unperturbed by layer 0's toggling");
    }

    // ---- M4: render() (summed) and renderPerTab() (per-layer) apply the
    //      SAME mute-ramp math -- both edited methods must agree, exactly
    //      like this codebase's own established render()/renderPerTab()
    //      cross-check (decktest.cpp) already does for everything else ----
    {
        const int len = 2000;
        const int muteFade = 50;

        Deck a, b;
        a.mode = b.mode = DeckMode::stem;
        for (int l = 0; l < kNumLayers; ++l)
        {
            auto ramp = makeRamp (len);
            a.layers[(size_t) l].left = ramp; a.layers[(size_t) l].loaded = true;
            b.layers[(size_t) l].left = ramp; b.layers[(size_t) l].loaded = true;
        }
        a.layers[0].muteFadeSamples = muteFade; a.layers[0].enabled = false;
        b.layers[0].muteFadeSamples = muteFade; b.layers[0].enabled = false;

        std::vector<float> sumL ((size_t) len), sumR ((size_t) len);
        a.render (sumL.data(), sumR.data(), len, 1.0f);

        std::array<std::vector<float>, kNumLayers> tabL, tabR;
        std::array<float*, kNumLayers> tabLPtr {}, tabRPtr {};
        for (int l = 0; l < kNumLayers; ++l)
        {
            tabL[(size_t) l].assign ((size_t) len, 0.0f);
            tabR[(size_t) l].assign ((size_t) len, 0.0f);
            tabLPtr[(size_t) l] = tabL[(size_t) l].data();
            tabRPtr[(size_t) l] = tabR[(size_t) l].data();
        }
        b.renderPerTab (tabLPtr, tabRPtr, len);

        bool matches = true;
        for (int i = 0; i < len; ++i)
        {
            float tabSumL = 0.0f;
            for (int l = 0; l < kNumLayers; ++l) tabSumL += tabL[(size_t) l][(size_t) i];
            if (! nearlyEqual ((double) sumL[(size_t) i], (double) tabSumL, 1e-4)) { matches = false; break; }
        }
        CHECK (matches, "render()'s and renderPerTab()'s mute-ramp math agree exactly -- summing renderPerTab()'s per-layer output (including the fading layer 0) reproduces render()'s pre-summed output");
    }

    std::printf ("\n%d/%d tests passed\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
