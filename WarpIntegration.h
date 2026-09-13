// ============================================================================
//  WarpIntegration.h — composes EzDSP.h's warpBeatsData + fitFourBars into a
//  single "re-warp this layer's audio to a new tempo" operation.
//
//  Pure C++, no JUCE, no Deck.h/Session.h dependency. This is a pure function
//  in the strict sense: it takes plain buffers and returns a new result
//  struct, never touches ezdeck::Layer, and knows nothing about the
//  cross-thread swap primitive that will eventually carry its output into a
//  live Layer (see ARCHITECTURE.md's resolved Architecture Decision Pending
//  #2 and Deck.h's Layer::stagePendingSwap). That separation is deliberate:
//  this file is safe to call from any thread, at any time, which is exactly
//  why the warp computation itself is proven correct here, in isolation,
//  before anything touches live audio-thread state (project/
//  MILESTONE_4_IMPLEMENTATION_PLAN.md, M4-T3).
//
//  EzDSP.h itself is frozen and is not modified by this file -- this composes
//  its existing, already-tested `analyze`, `warpBeatsData`, and `fitFourBars`
//  functions, exactly as called elsewhere in the app (e.g. Main.cpp's
//  loadLayer), rather than duplicating any of their logic.
// ============================================================================
#pragma once
#include "EzDSP.h"
#include <vector>
#include <cmath>

namespace ezdsp
{

struct ReWarpResult
{
    std::vector<float> left;
    std::vector<float> right;        // empty if the source had no right channel (mono)
    int    regionLength { 0 };       // newly-fitted 4-bar region length at targetBpm, in samples
    double bpm           { 120.0 };  // == targetBpm, unless fitFourBars' own octave-correction adjusted it
};

// Re-warps one layer's audio from sourceBpm to targetBpm, treating the WHOLE
// input buffer as the region to warp (s0 = 0, e0 = full duration) -- matching
// the rest of this codebase's existing "no region-start concept" limitation
// (see Main.cpp's WaveformView header comment); a future region-start field
// would change only what's passed as s0/e0 here, not this function's shape.
//
// `left` must be non-empty. `right` may be empty (mono source, matching
// ezdeck::Layer's own convention where an empty `right` means "duplicate
// left"). Both channels are warped using the SAME detected transient list
// (from `left` only) so left/right stay slice-aligned -- analyzing each
// channel independently could pick slightly different transients per
// channel and desync the stereo image.
//
// A no-op warp (sourceBpm == targetBpm, within floating-point tolerance)
// returns the input audio unchanged and only re-fits the region -- skipping
// warpBeatsData entirely avoids any needless rounding/fade-blend artifacts
// from a warp that wouldn't change the tempo anyway.
inline ReWarpResult reWarpLayer (const std::vector<float>& left,
                                  const std::vector<float>& right,
                                  double sampleRate,
                                  double sourceBpm,
                                  double targetBpm)
{
    ReWarpResult result;
    if (left.empty() || sampleRate <= 0.0 || sourceBpm <= 0.0 || targetBpm <= 0.0)
        return result;   // defensive no-op on nonsensical input -- caller decides how to handle an empty result

    const int    n        = (int) left.size();
    const double duration = (double) n / sampleRate;

    if (std::fabs (sourceBpm - targetBpm) < 1e-9)
    {
        result.left  = left;
        result.right = right;
        auto fit = fitFourBars (duration, targetBpm);
        result.regionLength = (int) std::llround (fit.end * sampleRate);
        result.bpm = fit.bpm;
        return result;
    }

    const auto onsets = analyze (left.data(), n, sampleRate).transients;

    auto warpedLeft = warpBeatsData (left.data(), n, sampleRate, 0.0, duration,
                                      sourceBpm, targetBpm, onsets);
    result.left = std::move (warpedLeft.out);

    if (! right.empty())
    {
        auto warpedRight = warpBeatsData (right.data(), (int) right.size(), sampleRate,
                                           0.0, duration, sourceBpm, targetBpm, onsets);
        result.right = std::move (warpedRight.out);
    }

    const double newDuration = (double) result.left.size() / sampleRate;
    auto fit = fitFourBars (newDuration, targetBpm);
    result.regionLength = (int) std::llround (fit.end * sampleRate);
    result.bpm = fit.bpm;
    return result;
}

} // namespace ezdsp
