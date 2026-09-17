// ============================================================================
//  Stretch.h -- changing a song's tempo without changing its pitch.
//
//  Owner: "I want to change the tempo on the fly." The old stretch
//  (EzDSP.h warpBeatsData) moved drum-hit slices to a new grid: fine for a
//  drum loop, but vocals and keys got gaps when slowed down, and only the
//  first 45 seconds were analysed.
//
//  This is WSOLA (waveform-similarity overlap-add): the output is built from
//  short overlapping pieces of the input (42 ms, half-overlapping, Hann
//  windowed), each taken from near where it "should" come from, nudged by up
//  to 10 ms to the spot whose waveform best continues the previous piece --
//  so there are no clicks, no gaps and no pitch change.
//
//  Stems stay locked together: the nudges are decided ONCE, from a mix of
//  every stem (plan()), and the same plan is applied to each stem (apply()).
//  The output is exactly round(n * scale) samples long, so a position p in
//  the original is at p * scale in the result, give or take the nudge.
//
//  Pure C++, no JUCE. Not real-time: a worker thread runs it.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ezstretch
{

/** Where each output piece is taken from, decided once for a whole song. */
struct Plan
{
    double scale { 1.0 };              // output length / input length (tempo down -> > 1)
    int    frame { 0 };                // piece length, samples
    int    hop   { 0 };                // output hop (frame / 2)
    int64_t inputLength  { 0 };
    int64_t outputLength { 0 };
    std::vector<int64_t> sourceStart;  // input sample where output piece k begins
};

namespace detail
{
    inline int frameFor (double sampleRate)
    {
        int f = 1;
        while (f < (int) (sampleRate * 0.042)) f <<= 1;   // ~42 ms, a power of two
        return std::max (256, f);
    }
}

/** Decides the plan from `guide` (a mono mix of every stem). */
inline Plan plan (const float* guide, int64_t n, double sampleRate, double scale)
{
    Plan p;
    p.scale = std::clamp (scale, 0.25, 4.0);
    p.frame = detail::frameFor (sampleRate);
    p.hop   = p.frame / 2;
    p.inputLength  = n;
    p.outputLength = (int64_t) std::llround ((double) n * p.scale);
    if (n <= 0) return p;

    const int tolerance = std::max (1, (int) (sampleRate * 0.010));   // +/- 10 ms
    const int decim = std::max (1, (int) std::lround (sampleRate / 6000.0));   // compare at ~6 kHz
    const int cmpLen = p.frame / decim;
    const double inputHop = (double) p.hop / p.scale;

    // decimated guide, for the similarity search
    std::vector<float> small ((size_t) (n / decim + 1), 0.0f);
    for (int64_t i = 0; i < n; ++i) small[(size_t) (i / decim)] += guide[i];

    auto at = [&small] (int64_t i) { return (i >= 0 && (size_t) i < small.size()) ? small[(size_t) i] : 0.0f; };

    const int64_t pieces = p.outputLength / p.hop + 2;
    p.sourceStart.resize ((size_t) pieces);
    int64_t prevSource = 0;
    for (int64_t k = 0; k < pieces; ++k)
    {
        const int64_t nominal = (int64_t) std::llround ((double) k * inputHop);
        if (k == 0) { p.sourceStart[0] = 0; prevSource = 0; continue; }

        // the input that would naturally follow the previous piece
        const int64_t natural = prevSource + p.hop;
        const int64_t lo = std::max<int64_t> (0, nominal - tolerance);
        const int64_t hi = std::min<int64_t> (std::max<int64_t> (0, n - 1), nominal + tolerance);

        int64_t best = std::clamp (nominal, lo, std::max (lo, hi));
        double bestScore = -1.0e300;
        const int64_t nat = natural / decim;
        double natEnergy = 0.0;
        for (int j = 0; j < cmpLen; ++j) { const double v = at (nat + j); natEnergy += v * v; }
        if (natEnergy > 1.0e-12)
        {
            for (int64_t c = lo / decim; c <= hi / decim; ++c)
            {
                double dot = 0.0, energy = 0.0;
                for (int j = 0; j < cmpLen; ++j)
                {
                    const double a = at (nat + j), b = at (c + j);
                    dot += a * b;
                    energy += b * b;
                }
                const double score = energy > 1.0e-12 ? dot / std::sqrt (energy) : -1.0e300;
                if (score > bestScore) { bestScore = score; best = c * decim; }
            }
            best = std::clamp (best, lo, std::max (lo, hi));
        }
        p.sourceStart[(size_t) k] = best;
        prevSource = best;
    }
    return p;
}

/** Stretches one channel by `plan`. The result is plan.outputLength long. */
inline std::vector<float> apply (const Plan& p, const float* input, int64_t n)
{
    std::vector<float> out ((size_t) std::max<int64_t> (0, p.outputLength), 0.0f);
    if (p.frame <= 0 || n <= 0 || out.empty()) return out;

    std::vector<float> window ((size_t) p.frame);
    for (int i = 0; i < p.frame; ++i)   // periodic Hann: halves overlapping at 50% sum to exactly 1
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / p.frame));

    const int64_t outLen = (int64_t) out.size();
    for (size_t k = 0; k < p.sourceStart.size(); ++k)
    {
        const int64_t outStart = (int64_t) k * p.hop - p.hop;   // the first piece's first half is the fade-in
        const int64_t src = p.sourceStart[k] - p.hop;
        for (int i = 0; i < p.frame; ++i)
        {
            const int64_t o = outStart + i;
            if (o < 0 || o >= outLen) continue;
            const int64_t s = src + i;
            const float v = (s >= 0 && s < n) ? input[s] : 0.0f;
            out[(size_t) o] += v * window[(size_t) i];
        }
    }
    return out;
}

} // namespace ezstretch
