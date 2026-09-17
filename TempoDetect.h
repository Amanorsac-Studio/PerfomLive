// ============================================================================
//  TempoDetect.h -- what tempo a loop or a song is played at.
//
//  Owner: "improve the tempo detection algorithm, what we have is not perfect
//  koraa." The old reading (EzDSP.h tempoOf) looked only at the first channel,
//  weighed every tempo towards 120, resolved to about 1.4 BPM and had no way
//  to tell 70 from 140 -- so the slow worship loops the owner uses (65-80)
//  came out at double time.
//
//  How this one listens:
//    1. Onset strength: a spectral flux of the log spectrum (both channels
//       summed), every 5 ms, with its slow trend removed -- how much NEW sound
//       starts at each moment, in any band, not just how loud it is.
//    2. Periodicity: the autocorrelation of that curve, normalised for how
//       many frames each lag compares. A candidate tempo scores the sum of
//       the correlation at 1..4 of its beats, so a beat that keeps repeating
//       beats a lucky single match.
//    3. Precision: the best candidate is refined against up to 16 of its
//       beats (a long ruler), to a few hundredths of a BPM.
//    4. Half or double: at the slower reading, are the "ands" (the faster
//       reading's other beats) as strong as the beats? A backbeat song keeps
//       its fast reading; a slow groove with hi-hat eighths keeps the slow one.
//    5. A short file is taken to be a loop: the tempo is nudged so the file
//       holds a whole number of beats, and a reading within 0.15 of a whole
//       number is taken as that number (loops are made at whole tempos).
//
//  Pure C++ (no JUCE), tested by tempotest.cpp. Not real-time: runs on a
//  worker thread or behind a menu choice.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

namespace eztempo
{

struct Result
{
    double bpm { 0.0 };            // 0 = no steady beat found
    double confidence { 0.0 };     // 0..1: how clearly one tempo stood out
    double half { 0.0 };           // the same groove felt at half speed (0 if out of range)
    double doubled { 0.0 };        // ... and at double speed
    double firstBeatSeconds { 0.0 };
};

struct Options
{
    double minBpm { 50.0 };
    double maxBpm { 200.0 };
    double maxAnalysisSeconds { 90.0 };
    double loopSnapMaxSeconds { 64.0 };   // files this short are treated as loops

    // How the reading is chosen. Tuned on the owner's own material (179 named
    // loops and stems, 17 Sep 2026): these settings read 114 exactly, against
    // 64 for the first version of this file; drums and percussion, which
    // Detect tempo listens to first, were right nearly every time.
    double priorCentre  { 110.0 };   // the tempo a reading leans towards...
    double priorOctaves { 0.6 };     // ...and how firmly (wider = weaker)
    int    combBeats    { 8 };       // beats compared for the first pick
    int    refineBeats  { 16 };      // beats compared for the precise value
    double pool         { 0.02 };    // tolerance for a drifting (live) tempo, as a fraction of a beat
    int    familyBeats  { 0 };       // >0: weigh 1/2, 2, 2/3, 3/2 of the pick over this many beats
    int    octaveRule   { 0 };       // 1: low-band "and" check for half/double (hurt on real music)
    double halveBelow   { 0.35 };
    double doubleAbove  { 0.6 };
};

namespace detail
{
    constexpr double kPi = 3.14159265358979323846;

    /** In-place FFT; `roots` caches the unit roots for this size (filled on first use). */
    inline void fft (std::vector<std::complex<double>>& a, std::vector<std::complex<double>>& roots)
    {
        const size_t n = a.size();
        if (roots.size() != n / 2)
        {
            roots.resize (n / 2);
            for (size_t k = 0; k < n / 2; ++k)
                roots[k] = std::polar (1.0, -2.0 * kPi * (double) k / (double) n);
        }
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap (a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const size_t half = len / 2, step = n / len;
            for (size_t i = 0; i < n; i += len)
                for (size_t k = 0; k < half; ++k)
                {
                    const auto u = a[i + k];
                    const auto v = a[i + k + half] * roots[k * step];
                    a[i + k] = u + v;
                    a[i + k + half] = u - v;
                }
        }
    }

    inline void fft (std::vector<std::complex<double>>& a)
    {
        std::vector<std::complex<double>> roots;
        fft (a, roots);
    }

    inline size_t nextPow2 (size_t v) { size_t p = 1; while (p < v) p <<= 1; return p; }

    /** Removes the slow trend (a swell, a fade) so only fresh attacks remain. */
    inline std::vector<double> detrend (const std::vector<double>& in, double fps)
    {
        const int half = std::max (1, (int) std::lround (fps * 0.2));
        std::vector<double> prefix (in.size() + 1, 0.0);
        for (size_t i = 0; i < in.size(); ++i) prefix[i + 1] = prefix[i] + in[i];
        std::vector<double> out (in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            const size_t a = i >= (size_t) half ? i - (size_t) half : 0;
            const size_t b = std::min (in.size(), i + (size_t) half + 1);
            const double mean = (prefix[b] - prefix[a]) / (double) (b - a);
            out[i] = std::max (0.0, in[i] - mean);
        }
        return out;
    }

    /** Spectral-flux onset strength, one value per hop (5 ms): `full` over the
        whole spectrum (for the tempo), `low` below 1.5 kHz (kick, snare body,
        bass -- for telling a real beat from a hi-hat subdivision). */
    struct Onsets { std::vector<double> full, low; };

    inline Onsets onsetStrength (const float* mono, int n, double sr, double& fps)
    {
        const int hop = std::max (1, (int) std::lround (sr * 0.005));
        const int win = (int) nextPow2 ((size_t) std::lround (sr * 0.046));
        fps = sr / hop;
        Onsets out;
        if (n < win) return out;

        std::vector<double> window ((size_t) win);
        for (int i = 0; i < win; ++i) window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * i / (win - 1));

        const int maxBin = std::min (win / 2, (int) (11000.0 / sr * win));
        const int lowBin = std::max (2, (int) (1500.0 / sr * win));
        std::vector<double> prev ((size_t) maxBin, 0.0), cur ((size_t) maxBin, 0.0);
        std::vector<std::complex<double>> buf ((size_t) win), roots;

        const int frames = 1 + (n - win) / hop;
        std::vector<double> full, low;
        full.reserve ((size_t) frames);
        low.reserve ((size_t) frames);
        for (int f = 0; f < frames; ++f)
        {
            const int start = f * hop;
            for (int i = 0; i < win; ++i) buf[(size_t) i] = { (double) mono[start + i] * window[(size_t) i], 0.0 };
            fft (buf, roots);
            double fluxAll = 0.0, fluxLow = 0.0;
            for (int b = 1; b < maxBin; ++b)
            {
                cur[(size_t) b] = std::log1p (1000.0 * std::abs (buf[(size_t) b]) / win);
                const double d = cur[(size_t) b] - prev[(size_t) b];
                if (d > 0.0)
                {
                    fluxAll += b < lowBin ? 1.5 * d : d;   // a little extra for kick and bass
                    if (b < lowBin) fluxLow += d;
                }
            }
            full.push_back (f == 0 ? 0.0 : fluxAll);
            low.push_back (f == 0 ? 0.0 : fluxLow);
            std::swap (prev, cur);
        }
        out.full = detrend (full, fps);
        out.low  = detrend (low, fps);
        return out;
    }

    /** Autocorrelation for lags 0..maxLag, each divided by how many pairs it
        compares, then by lag 0. */
    inline std::vector<double> autocorrelation (const std::vector<double>& x, int maxLag)
    {
        const size_t n = x.size();
        const size_t size = nextPow2 (2 * n);
        const double mean = std::accumulate (x.begin(), x.end(), 0.0) / (double) std::max<size_t> (1, n);
        std::vector<std::complex<double>> a (size, 0.0);
        for (size_t i = 0; i < n; ++i) a[i] = x[i] - mean;
        fft (a);
        for (auto& v : a) v = std::norm (v);
        fft (a);   // the forward transform again: the real part is the autocorrelation, reversed and scaled
        std::vector<double> r ((size_t) maxLag + 1, 0.0);
        for (int lag = 0; lag <= maxLag && (size_t) lag < n; ++lag)
        {
            const size_t idx = lag == 0 ? 0 : size - (size_t) lag;
            r[(size_t) lag] = a[idx].real() / (double) (n - (size_t) lag);
        }
        const double r0 = r[0] > 0.0 ? r[0] : 1.0;
        for (auto& v : r) v /= r0;
        return r;
    }

    inline double at (const std::vector<double>& r, double lag)
    {
        if (lag < 0.0) return 0.0;
        const size_t i = (size_t) lag;
        if (i + 1 >= r.size()) return 0.0;
        const double f = lag - (double) i;
        return r[i] * (1.0 - f) + r[i + 1] * f;
    }

    /** How strongly a tempo repeats: the correlation at 1..beats of its beats,
        each the best within +/- pool of a beat (a drifting live tempo). */
    inline double comb (const std::vector<double>& r, double fps, double bpm, int beats, double pool = 0.0)
    {
        const double lag = 60.0 * fps / bpm;
        double sum = 0.0;
        int used = 0;
        for (int k = 1; k <= beats; ++k)
        {
            const double centre = k * lag;
            if (centre >= (double) r.size() - 1) break;
            double v = at (r, centre);
            if (pool > 0.0)
            {
                const double reach = std::max (1.0, pool * lag);
                for (double x = centre - reach; x <= centre + reach; x += 1.0) v = std::max (v, at (r, x));
            }
            sum += v;
            ++used;
        }
        return used == 0 ? 0.0 : sum / used;
    }

    /** At this tempo's best phase: mean strength on the beats, mean strength
        halfway between them, and where the first beat falls (in frames). */
    struct Pulse { double onBeat { 0.0 }, offBeat { 0.0 }, phase { 0.0 }; };

    inline Pulse pulse (const std::vector<double>& o, double fps, double bpm)
    {
        Pulse best;
        const double period = 60.0 * fps / bpm;
        const int steps = std::max (1, (int) period);
        auto peakNear = [&o] (double pos)
        {
            const long c = std::lround (pos);
            double m = 0.0;
            for (long i = c - 2; i <= c + 2; ++i)
                if (i >= 0 && (size_t) i < o.size()) m = std::max (m, o[(size_t) i]);
            return m;
        };
        for (int s = 0; s < steps; ++s)
        {
            double on = 0.0, off = 0.0;
            int count = 0;
            for (double t = s; t + period * 0.5 < (double) o.size(); t += period)
            {
                on += peakNear (t);
                off += peakNear (t + period * 0.5);
                ++count;
            }
            if (count == 0) continue;
            on /= count; off /= count;
            if (on > best.onBeat) best = { on, off, (double) s };
        }
        return best;
    }
}

/** The expensive part of a reading, done once per file (decide() is cheap). */
struct Analysis
{
    bool   valid { false };
    double fps { 0.0 }, sampleRate { 0.0 }, seconds { 0.0 };
    int    firstSample { 0 };
    std::vector<double> onsets, low, acf;
};

/** Listens to `mono` (the channels summed). `totalSeconds` is the whole file's
    length, used to decide whether it is a loop; 0 = the length given. */
inline Analysis analyse (const float* mono, int n, double sampleRate, double totalSeconds = 0.0, double maxAnalysisSeconds = 90.0)
{
    using namespace detail;
    Analysis a;
    if (mono == nullptr || n <= 0 || sampleRate <= 0.0) return a;
    a.sampleRate = sampleRate;
    a.seconds = totalSeconds > 0.0 ? totalSeconds : (double) n / sampleRate;

    // start at the first sound, and look at no more than maxAnalysisSeconds
    int first = 0;
    while (first < n && std::fabs (mono[first]) < 1.0e-4f) ++first;
    a.firstSample = first;
    const int count = std::min (n - first, (int) (maxAnalysisSeconds * sampleRate));
    if (count < (int) (sampleRate * 2.0)) return a;   // under two seconds: no tempo to hear

    auto onsets = onsetStrength (mono + first, count, sampleRate, a.fps);
    if (onsets.full.size() < 64) return a;
    if (*std::max_element (onsets.full.begin(), onsets.full.end()) <= 1.0e-9) return a;

    // long enough for 16 beats at 40 BPM, whatever the options ask later
    const int maxLag = std::min ((int) onsets.full.size() - 2, (int) std::lround (16.0 * 60.0 * a.fps / 40.0));
    a.acf = autocorrelation (onsets.full, maxLag);
    a.onsets = std::move (onsets.full);
    a.low = std::move (onsets.low);
    a.valid = true;
    return a;
}

inline Result decide (const Analysis& a, const Options& opt = {})
{
    using namespace detail;
    Result res;
    if (! a.valid) return res;
    const double fps = a.fps;
    const double sampleRate = a.sampleRate;
    const int first = a.firstSample;
    const auto& o   = a.onsets;
    const auto& low = a.low;
    const auto& r   = a.acf;

    // 1. coarse scan: the tempo that repeats best over four beats, with a
    //    gentle preference for the middle of the range (worship: 60-140)
    auto prior = [&opt] (double b) { return std::exp (-0.5 * std::pow (std::log2 (b / opt.priorCentre) / opt.priorOctaves, 2.0)); };
    std::vector<double> scores;
    double bestScore = -1.0, best = 0.0;
    for (double b = opt.minBpm; b <= opt.maxBpm; b += 0.1)
    {
        const double s = comb (r, fps, b, opt.combBeats, opt.pool);
        const double weighted = s * prior (b);
        scores.push_back (s);
        if (weighted > bestScore) { bestScore = weighted; best = b; }
    }
    if (bestScore <= 0.0) return res;

    // 2. fine: the same tempo against a long ruler
    auto refine = [&] (double around)
    {
        double top = -1.0, at2 = around;
        for (double b = around * 0.98; b <= around * 1.02; b += 0.005)
        {
            const double s = comb (r, fps, b, opt.refineBeats, opt.pool);
            if (s > top) { top = s; at2 = b; }
        }
        return at2;
    };
    best = refine (best);

    // 2b. the pick's relatives (half, double, two thirds, three halves),
    //     weighed over a longer stretch: the true beat keeps repeating
    if (opt.familyBeats > 0)
    {
        const double ratios[] = { 1.0, 0.5, 2.0, 2.0 / 3.0, 1.5 };
        double top = -1.0, pick = best;
        for (double k : ratios)
        {
            const double c = best * k;
            if (c < opt.minBpm || c > opt.maxBpm) continue;
            const double s = comb (r, fps, c, opt.familyBeats, opt.pool) * prior (c);
            if (s > top * (k == 1.0 ? 1.0 : 1.05)) { top = s; pick = c; }   // a relative must clearly win
        }
        if (pick != best) best = refine (pick);
    }

    // 3. half or double, judged in the low band (kick, snare body, bass): at
    //    the slower reading, a kick or snare on the "ands" means those are
    //    real beats (keep the fast reading); only hi-hat there means they are
    //    subdivisions (the slow reading is the tempo).
    const bool haveLow = ! low.empty() && *std::max_element (low.begin(), low.end()) > 1.0e-9;
    const auto& judge = haveLow ? low : o;
    auto andRatio = [&] (double bpm)
    {
        const auto p = pulse (judge, fps, bpm);
        return p.onBeat > 0.0 ? p.offBeat / p.onBeat : 1.0;
    };
    for (int guard = 0; guard < 3 && opt.octaveRule == 1; ++guard)
    {
        const double slower = best / 2.0;
        if (slower >= opt.minBpm && andRatio (slower) < opt.halveBelow
            && comb (r, fps, slower, 4) > 0.5 * comb (r, fps, best, 4))
        {
            best = refine (slower);
            continue;
        }
        const double faster = best * 2.0;
        if (faster <= opt.maxBpm && andRatio (best) > opt.doubleAbove
            && comb (r, fps, faster, 4) > 0.6 * comb (r, fps, best, 4))
        {
            best = refine (faster);
            continue;
        }
        break;
    }

    // 4. a short file is a loop: a whole number of beats, a whole-number tempo
    const double seconds = a.seconds;
    if (seconds <= opt.loopSnapMaxSeconds)
    {
        const double beats = seconds * best / 60.0;
        const double whole = std::round (beats);
        if (whole >= 2.0)
        {
            const double snapped = whole * 60.0 / seconds;
            if (std::fabs (snapped - best) / best < 0.006) best = snapped;
        }
    }
    const double rounded = std::round (best);
    best = std::fabs (best - rounded) < 0.15 ? rounded : std::round (best * 100.0) / 100.0;

    // confidence: how many spreads the chosen tempo stands above the rest
    double mean = 0.0, var = 0.0;
    for (double s : scores) mean += s;
    mean /= (double) scores.size();
    for (double s : scores) var += (s - mean) * (s - mean);
    const double sd = std::sqrt (var / (double) scores.size());
    const double chosen = comb (r, fps, best, opt.combBeats, opt.pool);
    res.confidence = sd > 0.0 ? std::clamp (((chosen - mean) / sd - 1.0) / 3.0, 0.0, 1.0) : 0.0;

    res.bpm = best;
    res.half = best / 2.0 >= 30.0 ? best / 2.0 : 0.0;
    res.doubled = best * 2.0 <= 300.0 ? best * 2.0 : 0.0;
    // A frame shows an attack a little before it sounds (the attack enters the
    // end of the 46 ms window first); fold the phase back into one beat.
    const double lead = 0.85 * (double) nextPow2 ((size_t) std::lround (sampleRate * 0.046)) / sampleRate;
    const double beatSeconds = 60.0 / best;
    const double phase = std::fmod (pulse (o, fps, best).phase / fps + lead, beatSeconds);
    res.firstBeatSeconds = (double) first / sampleRate + (phase > beatSeconds - 0.01 ? 0.0 : phase);
    return res;
}

/** Reads the tempo of `mono` (the channels summed). */
inline Result detect (const float* mono, int n, double sampleRate, double totalSeconds = 0.0, const Options& opt = {})
{
    return decide (analyse (mono, n, sampleRate, totalSeconds, opt.maxAnalysisSeconds), opt);
}

} // namespace eztempo
