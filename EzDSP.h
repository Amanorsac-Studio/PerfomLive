// ============================================================================
//  EzDSP.h  —  EzPlay's audio analysis + warp engine, ported from the
//  validated JavaScript reference implementation.
//
//  Pure C++: depends only on the standard library. No JUCE, no platform code.
//  This is deliberate — it keeps the hard-won math testable in isolation and
//  identical across every build target (standalone, VST, iOS, Android).
//
//  Ported functions (1:1 with the JS reference):
//    onsetEnv      — RMS onset envelope
//    peaksOf       — adaptive-threshold peak picking
//    tempoOf       — autocorrelation tempo, Ellis log-Gaussian weighting
//    refineOnsets  — sample-accurate attack refinement
//    analyze       — the full pass: transients + tempo + bars
//    warpBeatsData — beats-mode warp (tempo change without repitch)
//    fitFourBars   — tempo-based 4-bar region fitting
// ============================================================================
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

namespace ezdsp
{

struct OnsetEnvelope
{
    std::vector<float> on;   // half-wave-rectified onset strength per frame
    double fps { 0.0 };      // frames per second (sampleRate / hop)
};

struct AnalysisResult
{
    std::vector<double> transients;  // attack times in seconds
    double bpm        { 120.0 };
    int    bars       { 1 };
    double firstOnset { 0.0 };
};

struct WarpResult
{
    std::vector<float> out;   // warped audio
    int beats { 1 };
};

struct FitResult
{
    double bpm  { 120.0 };
    double end  { 0.0 };      // region end in seconds
    bool   exact { false };
};

// ---- RMS onset envelope ----------------------------------------------------
inline OnsetEnvelope onsetEnv (const float* d, int n, double sr,
                               int hop = 512, int win = 1024)
{
    OnsetEnvelope e;
    const int fr = (n - win) / hop;
    const int frames = std::max (0, fr);
    std::vector<float> en (frames, 0.0f);

    for (int f = 0; f < frames; ++f)
    {
        double s = 0.0;
        const int st = f * hop;
        for (int i = 0; i < win; ++i) { const double v = d[st + i]; s += v * v; }
        en[f] = (float) std::sqrt (s / win);
    }

    e.on.assign (en.size(), 0.0f);
    for (size_t f = 1; f < en.size(); ++f)
    {
        const float x = en[f] - en[f - 1];
        e.on[f] = x > 0.0f ? x : 0.0f;
    }
    e.fps = sr / hop;
    return e;
}

// ---- adaptive-threshold peak picking --------------------------------------
inline std::vector<double> peaksOf (const std::vector<float>& on, double fps,
                                    double minGapBpm = 320.0)
{
    std::vector<double> out;
    if (on.empty()) return out;

    double m = 0.0;
    for (float v : on) m += v;
    m /= (double) on.size();

    double va = 0.0;
    for (float v : on) va += (v - m) * (v - m);
    const double sd = std::sqrt (va / (double) on.size());

    const double th = m + sd;
    const int gap = (int) std::floor ((60.0 / minGapBpm) * fps);
    double last = -1e9;

    for (size_t f = 1; f + 1 < on.size(); ++f)
        if (on[f] > th && on[f] >= on[f - 1] && on[f] >= on[f + 1]
            && (double) f - last >= gap)
        {
            out.push_back ((double) f / fps);
            last = (double) f;
        }
    return out;
}

// ---- autocorrelation tempo, Ellis log-Gaussian weighting -------------------
inline double tempoOf (const std::vector<float>& on, double fps,
                       double lo = 70.0, double hi = 180.0, double ctr = 120.0)
{
    // SECURITY/robustness: `a` is a divisor below (bpm = 60*fps/lag). With a
    // very low frame rate (a file declaring an absurdly small sample rate)
    // floor() lands on 0, making bpm +inf, which propagates out as an
    // infinite "detected tempo" and then hits an inf->int cast (UB) in
    // analyze(). Clamp to at least 1 lag.
    const int a = std::max (1, (int) std::floor (fps * 60.0 / hi));
    const int b = std::max (a, (int) std::ceil  (fps * 60.0 / lo));
    double best = 0.0, bs = -1.0;

    for (int lag = a; lag <= b; ++lag)
    {
        double ac = 0.0;
        for (size_t f = lag; f < on.size(); ++f) ac += on[f] * on[f - lag];
        const double bpm = 60.0 * fps / lag;
        const double lg  = std::log2 (bpm / ctr) / 0.9;
        const double w   = std::exp (-0.5 * lg * lg);
        const double s   = ac * w;
        if (s > bs) { bs = s; best = bpm; }
    }
    return best;
}

// ---- sample-accurate attack refinement ------------------------------------
inline std::vector<double> refineOnsets (const float* data, int n, double sr,
                                         const std::vector<double>& coarse)
{
    std::vector<double> out;
    out.reserve (coarse.size());
    for (double t : coarse)
    {
        const int w0 = std::max (0, (int) std::round ((t - 0.03) * sr));
        const int w1 = std::min (n, (int) std::round ((t + 0.03) * sr));
        float peak = 0.0f;
        for (int i = w0; i < w1; ++i) { const float v = std::fabs (data[i]); if (v > peak) peak = v; }
        if (peak < 1e-4f) { out.push_back (t); continue; }
        const float th = peak * 0.25f;
        double refined = t;
        for (int i = w0; i < w1; ++i)
            if (std::fabs (data[i]) >= th) { refined = (double) i / sr; break; }
        out.push_back (refined);
    }
    return out;
}

// ---- full analysis pass ----------------------------------------------------
inline AnalysisResult analyze (const float* full, int n, double sr,
                               double maxSec = 45.0)
{
    AnalysisResult r;
    const double duration = (double) n / sr;
    const int an = duration > maxSec ? (int) std::round (maxSec * sr) : n;

    auto env = onsetEnv (full, an, sr);
    auto coarse = peaksOf (env.on, env.fps);
    r.transients = refineOnsets (full, an, sr, coarse);
    const double raw = tempoOf (env.on, env.fps);

    const double span = (double) an / sr;
    if (raw > 0.0)
    {
        const int beats = std::max (1, (int) std::round (span / (60.0 / raw)));
        r.bpm  = std::round (beats * 60.0 / span * 10.0) / 10.0;
        r.bars = std::max (1, (int) std::round (beats / 4.0));
    }
    r.firstOnset = r.transients.empty() ? 0.0 : r.transients.front();
    return r;
}

// ---- beats-mode warp: transient slices pinned to the target grid ----------
inline WarpResult warpBeatsData (const float* data, int n, double sr,
                                 double s0, double e0,
                                 double origBpm, double targetBpm,
                                 const std::vector<double>& transients)
{
    const double spbO = 60.0 / origBpm;
    const double spbN = 60.0 / targetBpm;
    const int beats = std::max (1, (int) std::round ((e0 - s0) / spbO));
    // Clamped: outN sizes an allocation and bounds every write below. An
    // extreme origBpm/targetBpm ratio could otherwise overflow the int cast
    // (UB) or ask for an unbounded buffer. 30 min at 192kHz, matching the
    // decode limit the loaders enforce.
    const double outNRaw = (double) beats * spbN * sr;
    const int outN = (int) std::max (1.0, std::min (outNRaw, 192000.0 * 60.0 * 30.0));

    WarpResult res;
    res.beats = beats;
    res.out.assign (outN, 0.0f);

    // build the cut list: s0 plus in-region transients, sorted & de-duped
    std::vector<double> cuts;
    cuts.push_back (s0);
    for (double t : transients)
        if (t > s0 + 1e-3 && t < e0 - 1e-3) cuts.push_back (t);
    std::sort (cuts.begin(), cuts.end());
    std::vector<double> dd; dd.push_back (cuts[0]);
    for (double t : cuts) if (t - dd.back() > 0.03) dd.push_back (t);
    cuts.swap (dd);

    struct Mark { double src; int dst; };
    std::vector<Mark> marks;
    double lastQ = -1.0;
    for (double a : cuts)
    {
        const double q = std::round (((a - s0) / spbO) * 4.0) / 4.0;
        if (q <= lastQ || q >= beats) continue;
        marks.push_back ({ a, (int) std::round (q * spbN * sr) });
        lastQ = q;
    }
    if (marks.empty()) marks.push_back ({ s0, 0 });

    const int fade = std::max (1, (int) std::round (0.005 * sr));
    for (size_t mi = 0; mi < marks.size(); ++mi)
    {
        const Mark& m = marks[mi];
        const double srcEnd = (mi + 1 < marks.size()) ? marks[mi + 1].src : e0;
        const int    dstEnd = (mi + 1 < marks.size()) ? marks[mi + 1].dst : outN;
        const int srcA   = (int) std::round (m.src * sr);
        const int srcLen = (int) std::round ((srcEnd - m.src) * sr);
        const int gap    = dstEnd - m.dst;
        const int copyLen = srcLen > gap + fade ? gap + fade : srcLen;

        for (int i = 0; i < copyLen; ++i)
        {
            const int di = m.dst + i;
            if (di >= outN) break;
            if (di < 0) continue;   // res.out[di] is a WRITE -- never let a negative mark index it
            const int si = srcA + i;
            float v = (si >= 0 && si < n) ? data[si] : 0.0f;
            if (copyLen < srcLen && i > copyLen - fade)
                v *= (float) (copyLen - i) / (float) fade;
            res.out[di] += v;
        }
    }
    return res;
}

// ---- tempo-based 4-bar region fitting -------------------------------------
inline FitResult fitFourBars (double duration, double detBpm)
{
    FitResult r;
    double bpm = std::max (40.0, std::min (260.0, detBpm > 0 ? detBpm : 120.0));
    auto beatsAt = [duration] (double b) { return duration / (60.0 / b); };

    const double b0 = beatsAt (bpm);
    if (b0 >= 8.0 && b0 <= 40.0)
    {
        double best = bpm, err = std::fabs (b0 - 16.0);
        const double muls[] = { 0.5, 2.0, 0.25, 4.0 };
        for (double mul : muls)
        {
            const double cand = detBpm * mul;
            if (cand < 40.0 || cand > 260.0) continue;
            const double e = std::fabs (beatsAt (cand) - 16.0);
            if (e < err - 1e-9) { err = e; best = cand; }
        }
        bpm = best;
    }
    bpm = std::round (bpm * 10.0) / 10.0;
    const double len = 16.0 * 60.0 / bpm;
    r.bpm  = bpm;
    r.end  = std::min (duration, len);
    r.exact = std::fabs (beatsAt (bpm) - 16.0) / 16.0 < 0.06;
    return r;
}

} // namespace ezdsp
