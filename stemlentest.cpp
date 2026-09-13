// ============================================================================
//  stemlentest.cpp — standalone console test for Deck.h's
//  ezdeck::resolveStemBarLength() (PLAN_ARRANGEMENT_VIEW.md Milestone 1).
//
//  Pure C++, no JUCE, no CMake, no audio device -- same convention as
//  dsptest.cpp/decktest.cpp. Proves the length handling by numbers:
//    - a 4-bar file still resolves to 4 bars (loop-mode's own historical
//      default length, still correct for stem mode too)
//    - a 16-bar file resolves to 16, not truncated to 4
//    - a 32-bar file resolves to 32 -- right at the cap, still accepted
//    - a file one bar over the cap (33 bars) is rejected, not silently
//      clamped to 32
//    - the resolution is tempo-independent: a 16-bar file at a slower tempo
//      (so its real duration is well past EzDSP.h's own internal 45-second
//      analysis window) still resolves to 16 -- proving this function
//      doesn't inherit ezdsp::AnalysisResult::bars' window-truncation bug
//      (see Deck.h's own comment on resolveStemBarLength for why that field
//      isn't used here)
//
//  Build & run (no CMake needed -- zero JUCE dependency):
//    cl /nologo /EHsc /std:c++17 /Fe:stemlentest.exe stemlentest.cpp
//    ./stemlentest.exe
// ============================================================================
#include "Deck.h"
#include <cmath>
#include <cstdio>

static int gPass = 0, gTotal = 0;

#define CHECK(cond, desc) do {                                    \
    ++gTotal;                                                     \
    if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); }    \
    else      {          std::printf ("  [FAIL] %s\n", desc); }    \
} while (0)

// A file of exactly `bars` bars at `bpm`, in seconds -- the same beatsAt()-
// style arithmetic resolveStemBarLength() and EzDSP.h's fitFourBars both use.
static double durationForBars (int bars, double bpm)
{
    return (double) bars * 4.0 * (60.0 / bpm);
}

int main()
{
    using namespace ezdeck;

    std::printf ("Deck.h stem-mode bar-length resolution tests (PLAN_ARRANGEMENT_VIEW.md M1)\n");

    // ---- a 4-bar file still resolves to 4 bars -----------------------------
    {
        const double dur = durationForBars (4, 120.0);   // 8.0s
        auto fit = resolveStemBarLength (dur, 120.0);
        CHECK (fit.accepted && fit.bars == 4,
               "a 4-bar file at 120bpm resolves to 4 bars, accepted");
    }

    // ---- a 16-bar file resolves to 16, NOT truncated to 4 ------------------
    {
        const double dur = durationForBars (16, 120.0);   // 32.0s
        auto fit = resolveStemBarLength (dur, 120.0);
        CHECK (fit.accepted && fit.bars == 16,
               "a 16-bar file at 120bpm resolves to 16 bars (not truncated to 4), accepted");
    }

    // ---- a 32-bar file resolves to 32 -- right at the cap, still accepted --
    {
        const double dur = durationForBars (32, 120.0);   // 64.0s
        auto fit = resolveStemBarLength (dur, 120.0);
        CHECK (fit.accepted && fit.bars == 32,
               "a 32-bar file at 120bpm resolves to 32 bars, accepted (right at the cap)");
    }

    // ---- a file one bar over the cap is rejected, not silently clamped -----
    {
        const double dur = durationForBars (33, 120.0);   // 66.0s
        auto fit = resolveStemBarLength (dur, 120.0);
        CHECK (! fit.accepted && fit.bars == 33,
               "a 33-bar file at 120bpm is rejected (accepted == false), and still reports its true 33 bars, not a clamped 32");
    }

    // ---- a much longer file is also rejected, not clamped to 32 ------------
    {
        const double dur = durationForBars (64, 120.0);   // 128.0s
        auto fit = resolveStemBarLength (dur, 120.0);
        CHECK (! fit.accepted && fit.bars == 64,
               "a 64-bar file is rejected, reporting its true 64 bars");
    }

    // ---- resolution is tempo-independent, unlike ezdsp::AnalysisResult::bars,
    //      which is capped to EzDSP.h's own 45-second analysis window -------
    {
        // 16 bars at 60bpm = 16 * 4 beats * (60/60)s/beat = 64.0s -- well past
        // the 45-second window analyze() itself would cap tempo detection to.
        // resolveStemBarLength takes duration/bpm directly (not raw audio),
        // so it has no such window and must still report the true 16 bars.
        const double dur = durationForBars (16, 60.0);   // 64.0s
        CHECK (dur > 45.0, "sanity check: this duration is deliberately past EzDSP.h's 45-second analysis window");
        auto fit = resolveStemBarLength (dur, 60.0);
        CHECK (fit.accepted && fit.bars == 16,
               "a 16-bar file at a slow tempo (64s, past the 45s analysis-window cap) still resolves to 16 bars, not under-reported");
    }

    // ---- an exactly-32-bar file at a different tempo is still accepted -----
    {
        const double dur = durationForBars (32, 174.0);   // fast tempo, ~44.1s -- near the boundary from the other side
        auto fit = resolveStemBarLength (dur, 174.0);
        CHECK (fit.accepted && fit.bars == 32,
               "a 32-bar file at 174bpm still resolves to 32 bars, accepted");
    }

    // ---- an unset/invalid bpm falls back to the function's own 120bpm
    //      default rather than dividing by zero or producing garbage -------
    {
        const double dur = durationForBars (4, 120.0);   // 8.0s, matches the 120bpm fallback exactly
        auto fit = resolveStemBarLength (dur, 0.0);
        CHECK (fit.accepted && fit.bars == 4,
               "bpm <= 0 falls back to 120bpm rather than dividing by zero");
    }

    std::printf ("\n%d/%d tests passed\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
