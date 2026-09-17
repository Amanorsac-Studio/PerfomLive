// ============================================================================
//  stretchtest.cpp -- standalone checks for Stretch.h (no JUCE).
//    g++ -std=c++17 -O2 stretchtest.cpp -o stretchtest && ./stretchtest
// ============================================================================
#include "Stretch.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int gPass = 0, gTotal = 0;
#define CHECK(cond, desc) do { ++gTotal; if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); } \
                               else std::printf ("  [FAIL] %s\n", desc); } while (0)

namespace
{
    constexpr double kSr = 48000.0;
    constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

    std::vector<float> sine (double hz, double seconds)
    {
        std::vector<float> x ((size_t) (seconds * kSr));
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) (0.5 * std::sin (kTwoPi * hz * (double) i / kSr));
        return x;
    }

    std::vector<float> clicks (double seconds, double every, double offset = 0.0)
    {
        std::vector<float> x ((size_t) (seconds * kSr), 0.0f);
        for (double t = offset; t < seconds; t += every)
        {
            const size_t at = (size_t) (t * kSr);
            for (size_t i = 0; i < 200 && at + i < x.size(); ++i)
                x[at + i] = (float) (std::exp (-(double) i / 40.0) * (i % 2 ? -1.0 : 1.0));
        }
        return x;
    }

    double pitchOf (const std::vector<float>& x, size_t from, size_t to)
    {
        int crossings = 0;
        for (size_t i = from + 1; i < to && i < x.size(); ++i)
            if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++crossings;
        return crossings / 2.0 / ((double) (to - from) / kSr);
    }

    double rms (const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
        return std::sqrt (s / (double) (to - from));
    }

    // where the loud onsets are, in seconds
    std::vector<double> onsets (const std::vector<float>& x)
    {
        std::vector<double> out;
        size_t quiet = 100000;
        for (size_t i = 0; i < x.size(); ++i)
        {
            if (std::fabs (x[i]) > 0.3f) { if (quiet > 2000) out.push_back ((double) i / kSr); quiet = 0; }
            else ++quiet;
        }
        return out;
    }
}

int main()
{
    std::printf ("Stretch.h\n");

    {
        const auto x = sine (440.0, 4.0);
        const auto p = ezstretch::plan (x.data(), (int64_t) x.size(), kSr, 1.25);   // slower: 25% longer
        const auto y = ezstretch::apply (p, x.data(), (int64_t) x.size());
        CHECK ((int64_t) y.size() == (int64_t) std::llround (x.size() * 1.25), "slowing down makes the audio exactly 25% longer");
        const double pitch = pitchOf (y, (size_t) (0.5 * kSr), (size_t) (4.5 * kSr));
        std::printf ("    440 Hz stretched reads %.1f Hz\n", pitch);
        CHECK (std::fabs (pitch - 440.0) < 3.0, "the pitch stays the same (440 Hz stays 440 Hz)");
        double lo = 1.0, hi = 0.0;
        for (double t = 0.2; t < 4.8; t += 0.1)
        {
            const double r = rms (y, (size_t) (t * kSr), (size_t) ((t + 0.05) * kSr));
            lo = std::min (lo, r); hi = std::max (hi, r);
        }
        std::printf ("    level range %.3f .. %.3f\n", lo, hi);
        CHECK (lo > 0.30 && hi < 0.40, "no gaps or dips: the level stays steady all the way through");
    }
    {
        const auto x = sine (330.0, 3.0);
        const auto p = ezstretch::plan (x.data(), (int64_t) x.size(), kSr, 0.8);   // faster
        const auto y = ezstretch::apply (p, x.data(), (int64_t) x.size());
        CHECK ((int64_t) y.size() == (int64_t) std::llround (x.size() * 0.8), "speeding up makes the audio exactly 20% shorter");
        CHECK (std::fabs (pitchOf (y, (size_t) (0.3 * kSr), (size_t) (2.1 * kSr)) - 330.0) < 3.0, "speeding up keeps the pitch too");
    }
    {
        // a click every half second (120 BPM) lands at t * scale
        const auto x = clicks (8.0, 0.5, 0.25);
        const double scale = 120.0 / 100.0;   // 120 -> 100 BPM
        const auto p = ezstretch::plan (x.data(), (int64_t) x.size(), kSr, scale);
        const auto y = ezstretch::apply (p, x.data(), (int64_t) x.size());
        const auto in = onsets (x), out = onsets (y);
        double worst = 0.0;
        size_t matched = 0;
        for (double t : in)
        {
            double nearest = 1.0e9;
            for (double u : out) nearest = std::min (nearest, std::fabs (u - t * scale));
            worst = std::max (worst, nearest);
            if (nearest < 0.012) ++matched;
        }
        std::printf ("    beats: %zu in, %zu out, worst placement %.1f ms\n", in.size(), out.size(), worst * 1000.0);
        CHECK (matched == in.size(), "every beat lands where the new tempo puts it (within 12 ms)");
        CHECK (out.size() == in.size(), "no beat is doubled or lost");
    }
    {
        // two different stems, one plan: they stay in step
        auto drums = clicks (6.0, 0.5);
        auto bass  = sine (55.0, 6.0);
        for (size_t i = 0; i < bass.size(); ++i) bass[i] *= (float) (0.5 + 0.5 * std::fabs (std::sin (kTwoPi * 1.0 * (double) i / kSr)));
        std::vector<float> mix (drums.size());
        for (size_t i = 0; i < mix.size(); ++i) mix[i] = drums[i] + bass[i];
        const auto p = ezstretch::plan (mix.data(), (int64_t) mix.size(), kSr, 1.1);
        const auto d = ezstretch::apply (p, drums.data(), (int64_t) drums.size());
        const auto b = ezstretch::apply (p, bass.data(), (int64_t) bass.size());
        const auto m = ezstretch::apply (p, mix.data(), (int64_t) mix.size());
        double err = 0.0;
        for (size_t i = 0; i < m.size(); ++i) err = std::max (err, (double) std::fabs (d[i] + b[i] - m[i]));
        CHECK (d.size() == b.size() && err < 1.0e-4, "stems stretched with one plan add up to the stretched mix: they stay in step");
    }
    {
        const auto x = sine (220.0, 2.0);
        const auto p = ezstretch::plan (x.data(), (int64_t) x.size(), kSr, 1.0);
        const auto y = ezstretch::apply (p, x.data(), (int64_t) x.size());
        double err = 0.0;
        for (size_t i = 2048; i + 2048 < x.size(); ++i) err = std::max (err, (double) std::fabs (y[i] - x[i]));
        CHECK (y.size() == x.size() && err < 1.0e-3, "no tempo change leaves the audio as it was");
    }
    {
        std::vector<float> empty;
        const auto p = ezstretch::plan (empty.data(), 0, kSr, 1.2);
        CHECK (ezstretch::apply (p, empty.data(), 0).empty(), "nothing in, nothing out");
    }

    std::printf ("\n%d/%d PASS\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
