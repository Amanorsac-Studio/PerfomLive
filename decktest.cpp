// ============================================================================
//  decktest.cpp — standalone console test for Deck.h.
//
//  Same idea as EzDSP's own test: pure C++, no JUCE, no audio device. Proves
//  the sample-lock invariant by numbers, not by ear:
//    - the render loop is block-size independent (playhead/output don't care
//      how a caller chunks samples into callbacks)
//    - each layer's phase is always exactly fmod(sharedPlayhead, layerLength),
//      even when layer lengths differ
//    - looping is sample-exact
//    - mixing is linear (toggling a layer only adds/removes that layer)
//    - a settable per-layer region length overrides the raw buffer length for
//      looping, without changing the phase-lock mechanism itself, and falls
//      back to the raw length when left unset (Milestone 2, M2-T1/M2-T2)
//    - a settable per-layer gain scales output exactly, and settable fade
//      in/out envelopes ramp linearly at the loop region's own boundaries;
//      defaults (gain 1.0, no fade) are output-identical to unset (Milestone
//      3, M3-T1/M3-T2)
//    - a deck's stem mode (Milestone 5, M5-T2) plays each layer through its
//      own numFrames() exactly once, no wraparound -- a layer past its own
//      length contributes silence rather than looping; stemLength()/
//      stemFinished() correctly reflect the longest loaded layer and
//      whether it's been reached; loop mode (the default) is provably
//      unchanged by any of this
//
//  Build & run (no CMake needed — this file has zero JUCE dependency):
//    g++ -std=c++17 -O2 decktest.cpp -o decktest.exe
//    ./decktest.exe
// ============================================================================
#include "Deck.h"
#include "WarpIntegration.h"   // SPEC_WARP_BUG_INVESTIGATION.md tests -- pure C++, no JUCE, safe here
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Global allocation counter (M4-T2's "no allocation on the swap path when
// nothing is pending" assertion) -- overriding operator new/delete for this
// whole standalone program is safe here since every other test in this file
// already just uses std::vector/std::printf, none of which care whether
// allocations are counted, only whether they still succeed (which they do,
// via std::malloc/std::free below).
static long gAllocCount = 0;
void* operator new (std::size_t sz)       { ++gAllocCount; return std::malloc (sz); }
void* operator new[] (std::size_t sz)     { ++gAllocCount; return std::malloc (sz); }
void  operator delete (void* p) noexcept  { std::free (p); }
void  operator delete[] (void* p) noexcept{ std::free (p); }

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

int main()
{
    using namespace ezdeck;

    std::printf ("Deck.h sample-alignment tests\n");

    // ---- block-size independence -------------------------------------
    {
        Deck a, b;
        auto ramp = makeRamp (997);              // prime length: no accidental alignment
        a.layers[0].left = ramp; a.layers[0].loaded = true;
        b.layers[0].left = ramp; b.layers[0].loaded = true;

        const int total = 2000;
        std::vector<float> aL (total), aR (total), bL (total), bR (total);

        a.render (aL.data(), aR.data(), total);           // one big block

        int done = 0;
        while (done < total)                              // many small, uneven blocks
        {
            const int chunk = std::min (37, total - done);
            b.render (bL.data() + done, bR.data() + done, chunk);
            done += chunk;
        }

        CHECK (nearlyEqual (a.playheadPosition(), b.playheadPosition()),
               "block-size independence: playhead matches regardless of chunking");
        CHECK (aL == bL && aR == bR,
               "block-size independence: sample output matches regardless of chunking");
    }

    // ---- phase alignment across mismatched layer lengths ----------------
    {
        Deck d;
        d.layers[0].left = makeRamp (1000); d.layers[0].loaded = true;   // long layer
        d.layers[1].left = makeRamp (400);  d.layers[1].loaded = true;   // shorter, same clock
        d.setRateRatio (1.0);

        std::vector<float> L (2500), R (2500);
        d.render (L.data(), R.data(), 2500);

        const double expectedPlayhead = 2500.0;
        CHECK (nearlyEqual (d.playheadPosition(), expectedPlayhead),
               "playhead advances 1:1 with samples at rateRatio 1.0");
        CHECK (nearlyEqual (d.phaseOf (0), std::fmod (expectedPlayhead, 1000.0)),
               "layer 0 (len 1000) phase matches independent fmod calculation");
        CHECK (nearlyEqual (d.phaseOf (1), std::fmod (expectedPlayhead, 400.0)),
               "layer 1 (len 400) phase matches independent fmod calculation");
    }

    // ---- exact loop point -------------------------------------------------
    {
        Deck d;
        const int len = 512;
        d.layers[0].left = makeRamp (len); d.layers[0].loaded = true;

        std::vector<float> L (len), R (len);
        d.render (L.data(), R.data(), len);   // exactly one full loop

        CHECK (nearlyEqual (d.phaseOf (0), 0.0),
               "after exactly one loop's worth of samples, phase returns to 0");
    }

    // ---- linear superposition (mixing correctness) -------------------------
    {
        const int len = 300;
        auto rampA = makeRamp (len);
        auto rampB = makeRamp (len);
        for (auto& v : rampB) v *= 2.0f;   // distinguishable content

        Deck soloA;
        soloA.layers[0].left = rampA; soloA.layers[0].loaded = true;

        Deck soloB;
        soloB.layers[1].left = rampB; soloB.layers[1].loaded = true;

        Deck both;
        both.layers[0].left = rampA; both.layers[0].loaded = true;
        both.layers[1].left = rampB; both.layers[1].loaded = true;

        std::vector<float> aL (len), aR (len), bL (len), bR (len), sumL (len), sumR (len);
        soloA.render (aL.data(), aR.data(), len, 1.0f);
        soloB.render (bL.data(), bR.data(), len, 1.0f);
        both.render (sumL.data(), sumR.data(), len, 1.0f);

        bool superposes = true;
        for (int i = 0; i < len; ++i)
            if (! nearlyEqual (sumL[(size_t) i], aL[(size_t) i] + bL[(size_t) i], 1e-5))
                { superposes = false; break; }

        CHECK (superposes,
               "mixed output equals the sum of each layer rendered alone (linear mixing)");
    }

    // ---- disabling a layer silences only that layer ------------------------
    {
        const int len = 200;
        Deck d;
        d.layers[0].left = makeRamp (len); d.layers[0].loaded = true;
        d.layers[1].left = makeRamp (len); d.layers[1].loaded = true;
        for (auto& v : d.layers[1].left) v *= 3.0f;
        d.layers[1].enabled = false;

        std::vector<float> L (len), R (len);
        d.render (L.data(), R.data(), len, 1.0f);

        Deck ref;
        ref.layers[0].left = d.layers[0].left; ref.layers[0].loaded = true;
        // layer 1 left unloaded here on purpose -> silent, matching the disabled layer

        std::vector<float> refL (len), refR (len);
        ref.render (refL.data(), refR.data(), len, 1.0f);

        CHECK (L == refL && R == refR,
               "a disabled layer contributes silence; output matches the other layer(s) alone");
    }

    // ---- region-bounded looping ---------------------------------------------
    {
        Deck d;
        const int rawLen = 1000, region = 300;
        d.layers[0].left = makeRamp (rawLen);
        d.layers[0].loaded = true;
        d.layers[0].regionLength = region;

        std::vector<float> L (310), R (310);
        d.render (L.data(), R.data(), 310);

        CHECK (nearlyEqual (d.phaseOf (0), std::fmod (310.0, (double) region)),
               "a region shorter than the raw buffer loops at the region length, not the raw length");
        CHECK (nearlyEqual ((double) L[305], 2.5, 1e-4),
               "output past the region boundary wraps back into the region (region, not raw length, governs looping)");
    }

    // ---- phase lock across differing raw lengths via a shared region ------
    {
        Deck d;
        d.layers[0].left = makeRamp (1000); d.layers[0].loaded = true; d.layers[0].regionLength = 250;
        d.layers[1].left = makeRamp (600);  d.layers[1].loaded = true; d.layers[1].regionLength = 250;
        d.setRateRatio (1.0);

        std::vector<float> L (900), R (900);
        d.render (L.data(), R.data(), 900);

        CHECK (nearlyEqual (d.phaseOf (0), d.phaseOf (1)),
               "two layers with different raw lengths but the same region length stay phase-locked");
        CHECK (nearlyEqual (d.phaseOf (0), std::fmod (900.0, 250.0)),
               "the shared region length, not either layer's raw length, determines the phase");
    }

    // ---- unset region falls back to full-buffer looping (regression) ------
    {
        Deck d;
        const int len = 512;
        d.layers[0].left = makeRamp (len); d.layers[0].loaded = true;
        // regionLength deliberately left at its default (0) -- must behave
        // exactly like Deck did before regionLength existed.

        std::vector<float> L (len), R (len);
        d.render (L.data(), R.data(), len);   // exactly one full raw-length loop

        CHECK (d.layers[0].regionLength == 0 && d.layers[0].loopLength() == d.layers[0].numFrames(),
               "an unset region resolves to the full raw buffer length");
        CHECK (nearlyEqual (d.phaseOf (0), 0.0),
               "with no region set, looping still happens at exactly the raw buffer length (backward-compatible)");
    }

    // ---- stale regionLength larger than the actual buffer (regression) ----
    // Reproduces a real crash: a live repro found regionLength holding a
    // value describing a LARGER buffer than the layer currently has (the
    // tempo re-warp pipeline can leave it stale relative to the buffer
    // that's actually loaded). Before the fix, this drove render()'s
    // fmod-derived index past the end of layer.left/right -- a real
    // std::vector out-of-bounds access (MSVC's debug CRT: "vector
    // subscript out of range"), not a hypothetical one.
    {
        Deck d;
        const int bufLen = 200;
        d.layers[0].left  = makeRamp (bufLen);
        d.layers[0].right = makeRamp (bufLen);
        d.layers[0].loaded = true;
        d.layers[0].regionLength = 500;   // stale -- larger than left/right's actual 200 samples

        CHECK (d.layers[0].loopLength() == bufLen,
               "loopLength() clamps a regionLength larger than the buffer down to the buffer's own size");

        // Render enough samples that fmod(playhead, regionLength) would, if
        // unclamped, walk past index 199 (e.g. positions 200-299) -- this is
        // exactly the range that indexed out of bounds before the fix.
        std::vector<float> L (400), R (400);
        d.render (L.data(), R.data(), 400, 1.0f);   // must not crash

        bool allFinite = true;
        for (int i = 0; i < 400; ++i)
            if (! std::isfinite (L[(size_t) i]) || ! std::isfinite (R[(size_t) i])) { allFinite = false; break; }
        CHECK (allFinite,
               "rendering past the buffer's own length with a stale, larger regionLength produces finite (in-bounds) output, not a crash or garbage");

        CHECK (d.phaseOf (0) < (double) bufLen,
               "phaseOf() also stays within the actual buffer length, never reading the stale larger region");
    }

    // ---- SPEC_WARP_BUG_INVESTIGATION.md root-cause fix: setRegionLengthClamped() ----
    // Root cause (see Deck.h's own comment on this method for the full
    // trace): the clip editor's "Fit 4 Bars"/region-end-drag handlers
    // (Main.cpp) compute a candidate regionLength from a duration read
    // BEFORE their own write commits; project/KNOWN_BUGS.md #13's
    // already-tracked, unsynchronized-writer race means a concurrently
    // applied re-warp swap (stagePendingSwap/applyPendingSwapIfAny, both
    // exercised for real below, not stubbed) can install a smaller buffer
    // in between, so the clip editor's since-stale, larger candidate wins
    // the plain-int field last -- reproducing the exact "~45% larger than
    // the layer's actual buffer" symptom the crash investigation found.
    // Neither reWarpLayer() nor fitFourBars() is at fault (proven below and
    // by direct inspection: fitFourBars' own `std::min (duration, len)`
    // already clamps to the POST-warp duration it's given).
    {
        Deck d;
        d.layers[0].left  = makeRamp (200);
        d.layers[0].right = makeRamp (200);
        d.layers[0].loaded = true;

        // The re-warp swap lands FIRST, shrinking this layer to 120 samples
        // with its own correctly-fitted regionLength of 110 -- via the
        // real M4-T2 primitives, not a raw field assignment.
        const bool staged = d.layers[0].stagePendingSwap (makeRamp (120), makeRamp (120), 110);
        CHECK (staged, "a re-warp swap stages cleanly onto a loaded layer");
        d.layers[0].applyPendingSwapIfAny();
        CHECK (d.layers[0].numFrames() == 120 && d.layers[0].regionLength == 110,
               "applying the staged swap installs the new (smaller) buffer and its correctly-fitted regionLength together");

        // The clip editor's candidate (170) was computed against the OLD
        // 200-sample duration BEFORE the swap above landed, but its write
        // reaches this layer AFTER -- exactly the dangerous ordering
        // KNOWN_BUGS.md #13 describes. Before this fix, a bare
        // `regionLength = 170` here would leave it ~42% larger than the
        // real 120-sample buffer, the same shape of mismatch as the
        // original crash's ~45%.
        d.layers[0].setRegionLengthClamped (170);
        CHECK (d.layers[0].regionLength == 120,
               "a stale, oversized candidate computed against a since-superseded buffer size is clamped to the buffer that's actually live right now, never left describing one that no longer exists");
        CHECK (d.layers[0].loopLength() == 120,
               "loopLength() and the STORED regionLength now agree -- correctness no longer depends on the read-side clamp alone");
    }
    {
        // setRegionLengthClamped(0) must keep 0's existing "unset -- loop
        // the full current buffer" sentinel meaning, not clamp it into some
        // other value.
        Deck d;
        d.layers[0].left = makeRamp (200);
        d.layers[0].loaded = true;
        d.layers[0].regionLength = 150;
        d.layers[0].setRegionLengthClamped (0);
        CHECK (d.layers[0].regionLength == 0 && d.layers[0].loopLength() == 200,
               "setRegionLengthClamped(0) resets to the unset sentinel, not a clamped-to-zero region");
    }

    // ---- Owner's START marker (Layer::regionStart): the region is
    // [regionStart, regionStart + loopLength). Every case here also proves
    // the backward-compatibility promise -- regionStart 0 behaves exactly as
    // before it existed. ----
    {
        Deck d;
        d.layers[0].left   = makeRamp (200);
        d.layers[0].loaded = true;

        CHECK (d.layers[0].regionStart == 0 && d.layers[0].regionStartClamped() == 0
                && d.layers[0].loopLength() == 200,
               "a fresh layer has no start marker and loops its whole buffer -- identical to pre-marker behaviour");

        // an unset regionLength means "to the end of the buffer FROM the start marker"
        d.layers[0].setRegionStartClamped (50);
        CHECK (d.layers[0].regionStart == 50 && d.layers[0].loopLength() == 150,
               "setting a start marker with no explicit length shortens the region to the remaining buffer");

        // an explicit length is measured FROM the start marker
        d.layers[0].setRegionLengthClamped (100);
        CHECK (d.layers[0].loopLength() == 100,
               "regionLength is measured from the start marker, not from sample 0");

        // start + length can never exceed the buffer
        d.layers[0].setRegionLengthClamped (500);
        CHECK (d.layers[0].regionStartClamped() + d.layers[0].loopLength() <= d.layers[0].numFrames(),
               "an oversized length is clamped so start+length always stays inside the buffer");

        // a start past the end is clamped to keep at least one sample
        d.layers[0].setRegionStartClamped (99999);
        CHECK (d.layers[0].regionStartClamped() == 199 && d.layers[0].loopLength() >= 1,
               "a start marker dragged past the end clamps to the last sample and never produces an empty region");

        // negative is refused too
        d.layers[0].setRegionStartClamped (-100);
        CHECK (d.layers[0].regionStartClamped() == 0,
               "a negative start marker clamps to the beginning of the buffer");
    }
    {
        // The audible half: with a start marker set, render() must read from
        // the marker, not from sample 0. A ramp buffer makes the read
        // position directly observable in the output value.
        Deck d;
        d.mode = DeckMode::loop;
        d.layers[0].left   = makeRamp (100);   // sample i == i/100
        d.layers[0].loaded = true;
        d.layers[0].setRegionStartClamped (40);
        d.layers[0].setRegionLengthClamped (20);   // region = samples [40, 60)

        std::vector<float> outL (4, 0.0f), outR (4, 0.0f);
        d.reset();
        d.render (outL.data(), outR.data(), 4, 1.0f);
        CHECK (nearlyEqual (outL[0], makeRamp (100)[40], 1e-4),
               "with a start marker set, playback begins AT the marker, not at sample 0");
        CHECK (nearlyEqual (outL[1], makeRamp (100)[41], 1e-4),
               "playback advances from the start marker one sample at a time");

        // wrap: after 20 samples the loop must return to the MARKER, not to 0
        std::vector<float> outL2 (24, 0.0f), outR2 (24, 0.0f);
        d.reset();
        d.render (outL2.data(), outR2.data(), 24, 1.0f);
        CHECK (nearlyEqual (outL2[20], makeRamp (100)[40], 1e-4),
               "the loop wraps back to the START MARKER, never to the discarded head of the file");
    }

    // ---- SPEC_WARP_BUG_INVESTIGATION.md: reWarpLayer's own regionLength stays
    // consistent with the buffer it installs, across several source->target
    // tempo pairs including a ~45%-scale change (proves the arithmetic side
    // of the investigation clean, separately from the write-race side above) ----
    {
        const double sr = 44100.0;
        const int    srcLen = 88200;   // 2.0s @ 44.1kHz
        const struct { double sourceBpm, targetBpm; } pairs[] = {
            { 120.0, 120.0 },   // no-op warp
            { 120.0,  90.0 },   // slower -- buffer grows
            {  90.0, 130.5 },   // faster -- buffer shrinks by roughly the ~45% case the investigation found
            { 174.0,  95.0 },
        };
        bool allConsistent = true;
        for (auto& p : pairs)
        {
            auto left  = makeRamp (srcLen);
            auto right = makeRamp (srcLen);
            auto warped = ezdsp::reWarpLayer (left, right, sr, p.sourceBpm, p.targetBpm);
            if (warped.left.empty()) { allConsistent = false; continue; }

            Deck d;
            d.layers[0].left = left; d.layers[0].right = right; d.layers[0].loaded = true;
            d.layers[0].stagePendingSwap (warped.left, warped.right, warped.regionLength);
            d.layers[0].applyPendingSwapIfAny();

            if (d.layers[0].regionLength > d.layers[0].numFrames()) allConsistent = false;
        }
        CHECK (allConsistent,
               "across several source->target tempo pairs (including a ~45%-scale change), reWarpLayer's own regionLength never describes a buffer larger than the one it actually installs");
    }

    // ---- gain scaling ---------------------------------------------------
    {
        const int len = 200;
        Deck plain, scaled;
        auto ramp = makeRamp (len);
        plain.layers[0].left  = ramp; plain.layers[0].loaded  = true;
        scaled.layers[0].left = ramp; scaled.layers[0].loaded = true;
        scaled.layers[0].gain = 2.0f;

        std::vector<float> pL (len), pR (len), sL (len), sR (len);
        plain.render  (pL.data(), pR.data(), len, 1.0f);
        scaled.render (sL.data(), sR.data(), len, 1.0f);

        bool scalesExactly = true;
        for (int i = 0; i < len; ++i)
            if (! nearlyEqual (sL[(size_t) i], pL[(size_t) i] * 2.0f, 1e-4))
                { scalesExactly = false; break; }

        CHECK (scalesExactly,
               "a non-unity gain scales output by exactly that factor");
    }

    // ---- fade-in ramp -----------------------------------------------------
    {
        const int len = 200, fadeIn = 50;
        Deck d;
        std::vector<float> constant ((size_t) len, 1.0f);   // constant amplitude source,
        d.layers[0].left = constant; d.layers[0].loaded = true;   // so the ramp shape is unambiguous
        d.layers[0].fadeInSamples = fadeIn;

        std::vector<float> L (len), R (len);
        d.render (L.data(), R.data(), len, 1.0f);

        bool rampsCorrectly = true;
        for (int i = 0; i < len; ++i)
        {
            const float expected = (i < fadeIn) ? (float) i / (float) fadeIn : 1.0f;
            if (! nearlyEqual (L[(size_t) i], expected, 1e-4))
                { rampsCorrectly = false; break; }
        }

        CHECK (rampsCorrectly,
               "fadeInSamples ramps linearly from 0 to full amplitude across exactly that many samples");
    }

    // ---- fade-out ramp ------------------------------------------------------
    {
        const int len = 200, fadeOut = 50;
        Deck d;
        std::vector<float> constant ((size_t) len, 1.0f);
        d.layers[0].left = constant; d.layers[0].loaded = true;
        d.layers[0].fadeOutSamples = fadeOut;

        std::vector<float> L (len), R (len);
        d.render (L.data(), R.data(), len, 1.0f);

        bool rampsCorrectly = true;
        for (int i = 0; i < len; ++i)
        {
            const float expected = (i > len - fadeOut) ? (float) (len - i) / (float) fadeOut : 1.0f;
            if (! nearlyEqual (L[(size_t) i], expected, 1e-4))
                { rampsCorrectly = false; break; }
        }

        CHECK (rampsCorrectly,
               "fadeOutSamples ramps linearly to 0 across exactly the last that-many samples");
    }

    // ---- default gain/fade equals no metadata set at all -------------------
    {
        const int len = 300;
        auto ramp = makeRamp (len);

        Deck untouched;
        untouched.layers[0].left = ramp; untouched.layers[0].loaded = true;
        // gain/fadeInSamples/fadeOutSamples deliberately left at their struct
        // defaults -- never assigned at all.

        Deck explicitDefaults;
        explicitDefaults.layers[0].left = ramp; explicitDefaults.layers[0].loaded = true;
        explicitDefaults.layers[0].gain           = 1.0f;
        explicitDefaults.layers[0].fadeInSamples  = 0;
        explicitDefaults.layers[0].fadeOutSamples = 0;

        std::vector<float> uL (len), uR (len), eL (len), eR (len);
        untouched.render        (uL.data(), uR.data(), len);
        explicitDefaults.render (eL.data(), eR.data(), len);

        CHECK (uL == eL && uR == eR,
               "explicitly-set default gain/fades produce output identical to never touching them");
    }

    // ---- M4-T2: pending-swap primitive applies exactly once ----------------
    {
        Layer layer;
        layer.left = makeRamp (100);
        layer.loaded = true;
        layer.regionLength = 0;

        auto newLeft  = makeRamp (50);
        auto newRight = makeRamp (50);
        for (auto& v : newRight) v *= 2.0f;   // distinguishable content

        const bool staged = layer.stagePendingSwap (newLeft, newRight, 40);
        CHECK (staged, "stagePendingSwap succeeds when no swap is already pending");
        CHECK (layer.pendingSwapReady.load(), "pendingSwapReady is true immediately after staging");

        layer.applyPendingSwapIfAny();
        CHECK (layer.left == newLeft && layer.right == newRight && layer.regionLength == 40,
               "applyPendingSwapIfAny() swaps in the staged buffers and region length");
        CHECK (! layer.pendingSwapReady.load(), "pendingSwapReady is false again immediately after applying");

        auto leftBefore = layer.left;
        auto rightBefore = layer.right;
        const int regionBefore = layer.regionLength;
        layer.applyPendingSwapIfAny();   // calling it again with nothing pending must be a pure no-op
        CHECK (layer.left == leftBefore && layer.right == rightBefore && layer.regionLength == regionBefore,
               "a second applyPendingSwapIfAny() call with nothing pending is a no-op (applies exactly once)");
    }

    // ---- M4-T2: at most one pending swap at a time -------------------------
    {
        Layer layer;
        layer.left = makeRamp (100);
        layer.loaded = true;

        auto firstLeft = makeRamp (10);
        CHECK (layer.stagePendingSwap (firstLeft, {}, 8),
               "the first stagePendingSwap on an idle layer succeeds");

        auto secondLeft = makeRamp (20);
        const bool secondStaged = layer.stagePendingSwap (secondLeft, {}, 16);
        CHECK (! secondStaged,
               "a second stagePendingSwap before the first is consumed is rejected (returns false)");

        layer.applyPendingSwapIfAny();
        CHECK (layer.left == firstLeft && layer.regionLength == 8,
               "the FIRST staged swap (not the rejected second one) is the one that gets applied");

        CHECK (layer.stagePendingSwap (secondLeft, {}, 16),
               "staging succeeds again once the previous swap has been consumed");
    }

    // ---- M4-T2: Deck::applyPendingSwaps() applies across every layer -------
    {
        Deck d;
        d.layers[0].left = makeRamp (10); d.layers[0].loaded = true;
        d.layers[1].left = makeRamp (10); d.layers[1].loaded = true;

        auto newLeft0 = makeRamp (5);
        auto newLeft1 = makeRamp (7);
        CHECK (d.layers[0].stagePendingSwap (newLeft0, {}, 5), "staging layer 0's swap succeeds");
        CHECK (d.layers[1].stagePendingSwap (newLeft1, {}, 7), "staging layer 1's swap succeeds");

        d.applyPendingSwaps();

        CHECK (d.layers[0].left == newLeft0 && d.layers[0].regionLength == 5,
               "Deck::applyPendingSwaps() applies layer 0's pending swap");
        CHECK (d.layers[1].left == newLeft1 && d.layers[1].regionLength == 7,
               "Deck::applyPendingSwaps() applies layer 1's pending swap in the same call");
    }

    // ---- M4-T2: render()'s existing behavior for a layer with no pending swap is unchanged ----
    {
        // Identical to this file's very first block, just re-run after the
        // swap primitive's fields were added to Layer -- proves the new
        // fields default to a no-op state and don't perturb existing
        // render() behavior at all when nothing is staged.
        Deck a, b;
        auto ramp = makeRamp (997);
        a.layers[0].left = ramp; a.layers[0].loaded = true;
        b.layers[0].left = ramp; b.layers[0].loaded = true;

        const int total = 2000;
        std::vector<float> aL (total), aR (total), bL (total), bR (total);
        a.render (aL.data(), aR.data(), total);
        int done = 0;
        while (done < total)
        {
            const int chunk = std::min (37, total - done);
            b.render (bL.data() + done, bR.data() + done, chunk);
            done += chunk;
        }
        CHECK (aL == bL && aR == bR,
               "render() output is bit-identical to before M4-T2 when no swap is ever staged (regression)");
    }

    // ---- M4-T2: no heap allocation when applying with nothing pending ------
    {
        Layer layer;
        layer.left = makeRamp (500);
        layer.loaded = true;

        const long before = gAllocCount;
        for (int i = 0; i < 1000; ++i) layer.applyPendingSwapIfAny();   // nothing ever staged
        const long after = gAllocCount;

        CHECK (after == before,
               "applyPendingSwapIfAny() performs zero heap allocations across 1000 calls when nothing is pending");
    }

    // ---- M5-T2: stem mode plays once, no wraparound ------------------------
    {
        Deck d;
        d.mode = DeckMode::stem;
        const int len = 300;
        d.layers[0].left = makeRamp (len);   // values 0..299
        d.layers[0].loaded = true;

        std::vector<float> L (len + 100), R (len + 100);   // render past the end
        d.render (L.data(), R.data(), len + 100, 1.0f);   // masterGain 1.0 -- comparing against absolute source values below

        bool silentPastEnd = true;
        for (int i = len; i < len + 100; ++i)
            if (L[(size_t) i] != 0.0f) { silentPastEnd = false; break; }

        CHECK (silentPastEnd,
               "stem mode: a layer contributes silence once playhead passes its own length (no wraparound)");
        CHECK (nearlyEqual ((double) L[(size_t) (len - 1)], (double) (len - 1), 1e-3),
               "stem mode: the last in-range sample matches the source content exactly (no fmod distortion)");
    }

    // ---- M5-T2: stemLength() reflects the longest LOADED layer -------------
    {
        Deck d;
        d.layers[0].left = makeRamp (200); d.layers[0].loaded = true;
        d.layers[1].left = makeRamp (500); d.layers[1].loaded = true;
        d.layers[2].left = makeRamp (900); d.layers[2].loaded = false;   // not loaded -- must not count

        CHECK (d.stemLength() == 500,
               "stemLength() returns the longest LOADED layer's length, ignoring a longer but unloaded one");
    }

    // ---- M5-T2: stemFinished() transitions exactly at stemLength() ---------
    {
        Deck d;
        d.mode = DeckMode::stem;
        const int len = 100;
        d.layers[0].left = makeRamp (len);
        d.layers[0].loaded = true;

        CHECK (! d.stemFinished(), "stemFinished() is false before any rendering (playhead 0 < stemLength)");

        std::vector<float> L (len - 1), R (len - 1);
        d.render (L.data(), R.data(), len - 1);
        CHECK (! d.stemFinished(), "stemFinished() is still false one sample before the end");

        std::vector<float> L2 (1), R2 (1);
        d.render (L2.data(), R2.data(), 1);   // playhead reaches exactly stemLength() after this sample
        CHECK (d.stemFinished(), "stemFinished() becomes true exactly once playhead reaches stemLength()");
    }

    // ---- M5-T2: loop mode is the default and is provably unaffected --------
    {
        Deck d;
        CHECK (d.mode == DeckMode::loop, "Deck defaults to loop mode");

        d.layers[0].left = makeRamp (100); d.layers[0].loaded = true;
        std::vector<float> L (10000), R (10000);
        d.render (L.data(), R.data(), 10000);   // far past any "length" -- loop mode must never finish

        CHECK (! d.stemFinished(),
               "a loop-mode deck never reports stemFinished(), no matter how long it's rendered");
    }

    // ---- M5-T2: mismatched-length layers in stem mode stop independently ---
    {
        Deck d;
        d.mode = DeckMode::stem;
        const int shortLen = 50, longLen = 150;
        auto shortRamp = makeRamp (shortLen);
        auto longRamp  = makeRamp (longLen);
        for (auto& v : longRamp) v *= 2.0f;   // distinguishable content

        d.layers[0].left = shortRamp; d.layers[0].loaded = true;
        d.layers[1].left = longRamp;  d.layers[1].loaded = true;

        std::vector<float> L (longLen), R (longLen);
        d.render (L.data(), R.data(), longLen);

        Deck longOnly;
        longOnly.mode = DeckMode::stem;
        longOnly.layers[0].left = longRamp; longOnly.layers[0].loaded = true;
        std::vector<float> refL (longLen), refR (longLen);
        longOnly.render (refL.data(), refR.data(), longLen);

        bool matchesAfterShortEnds = true;
        for (int i = shortLen; i < longLen; ++i)
            if (! nearlyEqual ((double) L[(size_t) i], (double) refL[(size_t) i], 1e-4))
                { matchesAfterShortEnds = false; break; }

        CHECK (matchesAfterShortEnds,
               "stem mode: a shorter layer silences itself while a longer layer in the same deck keeps playing");
        CHECK (d.stemLength() == longLen,
               "stemLength() reflects the longer of two differently-sized loaded layers");
    }

    // ---- M5-T2: fadeOutSamples still ramps toward the actual stem end ------
    {
        Deck d;
        d.mode = DeckMode::stem;
        const int len = 200, fadeOut = 50;
        std::vector<float> constant ((size_t) len, 1.0f);
        d.layers[0].left = constant; d.layers[0].loaded = true;
        d.layers[0].fadeOutSamples = fadeOut;

        std::vector<float> L (len), R (len);
        d.render (L.data(), R.data(), len, 1.0f);

        bool rampsCorrectly = true;
        for (int i = 0; i < len; ++i)
        {
            const float expected = (i > len - fadeOut) ? (float) (len - i) / (float) fadeOut : 1.0f;
            if (! nearlyEqual (L[(size_t) i], expected, 1e-4)) { rampsCorrectly = false; break; }
        }
        CHECK (rampsCorrectly,
               "stem mode: fadeOutSamples still ramps correctly toward the actual (non-looped) end of the stem");
    }

    // ---- M5-T2: phaseOf() clamps at the layer's length in stem mode --------
    {
        Deck d;
        d.mode = DeckMode::stem;
        const int len = 100;
        d.layers[0].left = makeRamp (len); d.layers[0].loaded = true;

        std::vector<float> L (len + 50), R (len + 50);
        d.render (L.data(), R.data(), len + 50);

        CHECK (nearlyEqual (d.phaseOf (0), (double) len),
               "phaseOf() clamps at the layer's own length in stem mode, rather than wrapping via fmod");
    }

    // ---- M7: renderPerTab() sums to the same result as render() (composition check) ----
    {
        Deck d;
        d.layers[0].left  = makeRamp (400);  d.layers[0].loaded = true; d.layers[0].gain = 0.7f;
        d.layers[1].left  = makeRamp (250);  d.layers[1].loaded = true; d.layers[1].gain = 1.3f;
        d.layers[2].loaded = false;   // deliberately empty -- must contribute exact silence
        d.layers[3].left  = makeRamp (600);  d.layers[3].loaded = true;

        const int n = 500;
        std::vector<float> refL (n), refR (n);
        // Deck/Layer are not copyable (Layer holds std::atomic members) -- build
        // an independent Deck with identical setup rather than copying `d`.
        Deck dRef;
        dRef.layers[0].left = makeRamp (400); dRef.layers[0].loaded = true; dRef.layers[0].gain = 0.7f;
        dRef.layers[1].left = makeRamp (250); dRef.layers[1].loaded = true; dRef.layers[1].gain = 1.3f;
        dRef.layers[2].loaded = false;
        dRef.layers[3].left = makeRamp (600); dRef.layers[3].loaded = true;
        dRef.render (refL.data(), refR.data(), n, 1.0f);   // masterGain 1.0 -- isolates renderPerTab's own summation

        std::array<std::vector<float>, ezdeck::kNumLayers> tabL, tabR;
        std::array<float*, ezdeck::kNumLayers> tabLPtr, tabRPtr;
        for (int i = 0; i < ezdeck::kNumLayers; ++i)
        {
            tabL[(size_t) i].assign ((size_t) n, 0.0f);
            tabR[(size_t) i].assign ((size_t) n, 0.0f);
            tabLPtr[(size_t) i] = tabL[(size_t) i].data();
            tabRPtr[(size_t) i] = tabR[(size_t) i].data();
        }
        d.renderPerTab (tabLPtr, tabRPtr, n);

        bool sumsMatch = true;
        for (int i = 0; i < n && sumsMatch; ++i)
        {
            float sumL = 0.0f, sumR = 0.0f;
            for (int li = 0; li < ezdeck::kNumLayers; ++li) { sumL += tabL[(size_t) li][(size_t) i]; sumR += tabR[(size_t) li][(size_t) i]; }
            if (std::fabs (sumL - refL[(size_t) i]) > 1e-5f || std::fabs (sumR - refR[(size_t) i]) > 1e-5f)
                sumsMatch = false;
        }
        CHECK (sumsMatch, "summing renderPerTab()'s 4 separate per-layer buffers reproduces render()'s single pre-mixed output exactly");

        bool layer2Silent = true;
        for (int i = 0; i < n; ++i)
            if (tabL[2][(size_t) i] != 0.0f || tabR[2][(size_t) i] != 0.0f) { layer2Silent = false; break; }
        CHECK (layer2Silent, "an unloaded layer's own tab buffer is exact silence, not just absent from the sum");

        bool layer0MatchesOwnGain = true;
        Deck isolated0;
        isolated0.layers[0].left = makeRamp (400); isolated0.layers[0].loaded = true; isolated0.layers[0].gain = 0.7f;
        std::vector<float> iso0L (n), iso0R (n);
        isolated0.render (iso0L.data(), iso0R.data(), n, 1.0f);
        for (int i = 0; i < n; ++i)
            if (std::fabs (tabL[0][(size_t) i] - iso0L[(size_t) i]) > 1e-5f) { layer0MatchesOwnGain = false; break; }
        CHECK (layer0MatchesOwnGain, "tab 0's own buffer exactly matches that layer rendered completely alone (no cross-layer leakage)");
    }

    // ---- M7: renderPerTab() is block-size independent, same as render() ----
    {
        auto runOnce = [&] (int chunkSize)
        {
            Deck d;
            d.layers[0].left = makeRamp (300); d.layers[0].loaded = true;
            d.layers[1].left = makeRamp (150); d.layers[1].loaded = true;

            const int n = 700;
            std::array<std::vector<float>, ezdeck::kNumLayers> tabL, tabR;
            std::array<float*, ezdeck::kNumLayers> tabLPtr, tabRPtr;
            for (int i = 0; i < ezdeck::kNumLayers; ++i)
            {
                tabL[(size_t) i].assign ((size_t) n, 0.0f);
                tabR[(size_t) i].assign ((size_t) n, 0.0f);
            }

            int done = 0;
            while (done < n)
            {
                const int chunk = std::min (chunkSize, n - done);
                for (int i = 0; i < ezdeck::kNumLayers; ++i)
                {
                    tabLPtr[(size_t) i] = tabL[(size_t) i].data() + done;
                    tabRPtr[(size_t) i] = tabR[(size_t) i].data() + done;
                }
                d.renderPerTab (tabLPtr, tabRPtr, chunk);
                done += chunk;
            }
            return tabL;
        };

        auto bigChunks   = runOnce (10'000);
        auto smallChunks = runOnce (37);

        CHECK (bigChunks[0] == smallChunks[0] && bigChunks[1] == smallChunks[1],
               "renderPerTab()'s output is bit-identical regardless of how render() calls are chunked into blocks");
    }

    std::printf ("\n%d/%d tests passed\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
