// ============================================================================
//  tempotest.cpp -- standalone checks for TempoDetect.h (no JUCE, no audio
//  device). Synthetic drum grooves at known tempos, built the way the owner's
//  loops are: kick, snare, hi-hats, sometimes a sixteenth shaker.
//
//    g++ -std=c++17 -O2 tempotest.cpp -o tempotest && ./tempotest
// ============================================================================
#include "TempoDetect.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static int gPass = 0, gTotal = 0;
#define CHECK(cond, desc) do { ++gTotal; if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); } \
                               else std::printf ("  [FAIL] %s\n", desc); } while (0)

namespace
{
    constexpr double kSr = 44100.0;
    constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

    void addKick (std::vector<float>& out, size_t at, float gain)
    {
        for (size_t i = 0; i < 9000 && at + i < out.size(); ++i)
        {
            const double t = (double) i / kSr;
            const double f = 50.0 + 90.0 * std::exp (-t * 30.0);
            out[at + i] += gain * (float) (std::sin (kTwoPi * f * t) * std::exp (-t * 9.0));
        }
    }

    // a hi-hat: noise with its lows removed (a real hat has almost nothing below 3 kHz)
    void addHat (std::vector<float>& out, size_t at, float gain, std::mt19937& rng)
    {
        std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        float x1 = 0, x2 = 0;
        for (size_t i = 0; i < 6000 && at + i < out.size(); ++i)
        {
            const float x = d (rng);
            const float hp = x - 2.0f * x1 + x2;   // second difference: a steep high-pass
            x2 = x1; x1 = x;
            out[at + i] += 0.5f * gain * hp * (float) std::exp (-(double) i / kSr * 60.0);
        }
    }

    // a snare: a low body tone under a burst of noise
    void addSnare (std::vector<float>& out, size_t at, float gain, std::mt19937& rng)
    {
        std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        for (size_t i = 0; i < 8000 && at + i < out.size(); ++i)
        {
            const double t = (double) i / kSr;
            const double body = std::sin (kTwoPi * 190.0 * t) * std::exp (-t * 25.0);
            out[at + i] += gain * (float) (0.6 * body + 0.5 * d (rng) * std::exp (-t * 18.0));
        }
    }

    struct Groove
    {
        bool kickOnAll4  { false };   // four on the floor
        bool backbeat    { true };    // kick 1 & 3, snare 2 & 4
        int  hatsPerBeat { 2 };       // 2 = eighths, 4 = sixteenths, 0 = none
        float hatGain    { 0.25f };
        double swing     { 0.0 };     // 0..0.33 delay of the off-beat eighth
    };

    std::vector<float> render (double bpm, double seconds, const Groove& g, unsigned seed = 7)
    {
        std::mt19937 rng (seed);
        std::vector<float> out ((size_t) (seconds * kSr), 0.0f);
        const double beat = 60.0 / bpm * kSr;
        for (int b = 0; b * beat < (double) out.size(); ++b)
        {
            const size_t at = (size_t) std::llround (b * beat);
            if (g.kickOnAll4 || (g.backbeat && b % 2 == 0)) addKick (out, at, 0.9f);
            if (g.backbeat && b % 2 == 1) addSnare (out, at, 0.7f, rng);
            for (int h = 0; h < g.hatsPerBeat; ++h)
            {
                double frac = (double) h / g.hatsPerBeat;
                if (g.hatsPerBeat == 2 && h == 1) frac += g.swing * 0.5;
                const size_t hat = (size_t) std::llround ((b + frac) * beat);
                addHat (out, hat, h == 0 ? g.hatGain : g.hatGain * 0.7f, rng);
            }
        }
        return out;
    }

    bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }

    double detectBpm (const std::vector<float>& x, double seconds)
    {
        return eztempo::detect (x.data(), (int) x.size(), kSr, seconds).bpm;
    }
}

int main()
{
    std::printf ("TempoDetect.h\n");

    {
        const double sec = 16.0 * 60.0 / 120.0;   // four bars at 120: a loop
        const auto x = render (120.0, sec, {});
        const double bpm = detectBpm (x, sec);
        std::printf ("    120 backbeat loop -> %.2f\n", bpm);
        CHECK (near (bpm, 120.0, 0.01), "a 120 BPM backbeat loop reads exactly 120");
    }
    {
        Groove g; g.hatsPerBeat = 4; g.hatGain = 0.35f;
        const double sec = 8.0 * 60.0 / 70.0 * 4.0;   // eight bars at 70 with sixteenth hats
        const auto x = render (70.0, sec, g);
        const double bpm = detectBpm (x, sec);
        std::printf ("    70 slow groove, sixteenth hats -> %.2f\n", bpm);
        // Tuned on real loops, the reading leans towards 110: a groove this slow
        // may come out at double time, and the ÷2 button is the answer then.
        CHECK (near (bpm, 70.0, 0.01) || near (bpm, 140.0, 0.01),
               "a slow 70 BPM groove with busy hats reads 70 or its double (140), never something unrelated");
    }
    {
        Groove g; g.hatsPerBeat = 2;
        const double sec = 8.0 * 60.0 / 75.0 * 4.0;
        const auto x = render (75.0, sec, g);
        const double bpm = detectBpm (x, sec);
        std::printf ("    75 worship loop, eighth hats -> %.2f\n", bpm);
        CHECK (near (bpm, 75.0, 0.01), "a 75 BPM loop with eighth hats reads 75, not 150");
    }
    {
        Groove g; g.hatsPerBeat = 2;
        const double sec = 16.0 * 60.0 / 141.0 * 4.0;
        const auto x = render (141.0, sec, g);
        const double bpm = detectBpm (x, sec);
        std::printf ("    141 highlife -> %.2f\n", bpm);
        CHECK (near (bpm, 141.0, 0.01), "a 141 BPM highlife groove reads 141, not 70.5");
    }
    {
        Groove g; g.backbeat = false; g.kickOnAll4 = true; g.hatsPerBeat = 2;
        const double sec = 16.0 * 60.0 / 128.0 * 4.0;
        const auto x = render (128.0, sec, g);
        const double bpm = detectBpm (x, sec);
        std::printf ("    128 four on the floor -> %.2f\n", bpm);
        CHECK (near (bpm, 128.0, 0.01), "a four-on-the-floor 128 reads 128");
    }
    {
        Groove g; g.hatsPerBeat = 2;
        const double sec = 90.0;   // a song, not a loop: no snapping to the file length
        const auto x = render (97.3, sec, g);
        const double bpm = detectBpm (x, sec);
        std::printf ("    97.3 song -> %.2f\n", bpm);
        CHECK (near (bpm, 97.3, 0.1), "a song at a fractional tempo (97.3) reads within 0.1 BPM");
    }
    {
        Groove g; g.hatsPerBeat = 2; g.swing = 0.3;
        const double sec = 8.0 * 60.0 / 100.0 * 4.0;
        const auto x = render (100.0, sec, g);
        const double bpm = detectBpm (x, sec);
        std::printf ("    100 swing -> %.2f\n", bpm);
        CHECK (near (bpm, 100.0, 0.01), "a swung 100 BPM groove reads 100");
    }
    {
        const auto x = render (65.0, 8.0 * 60.0 / 65.0 * 4.0, { false, true, 2, 0.2f, 0.0 });
        const auto r = eztempo::detect (x.data(), (int) x.size(), kSr);
        std::printf ("    65 -> %.2f (half %.2f, double %.2f, confidence %.2f)\n", r.bpm, r.half, r.doubled, r.confidence);
        const bool read65  = near (r.bpm, 65.0, 0.01)  && near (r.doubled, 130.0, 0.02);
        const bool read130 = near (r.bpm, 130.0, 0.01) && near (r.half, 65.0, 0.02);
        CHECK ((read65 || read130) && r.confidence > 0.2,
               "the result offers its half and double readings (one of them the true 65) and a confidence");
    }
    {
        std::vector<float> silence ((size_t) (kSr * 10.0), 0.0f);
        CHECK (eztempo::detect (silence.data(), (int) silence.size(), kSr).bpm == 0.0, "silence has no tempo");
        std::vector<float> tiny ((size_t) (kSr * 1.0), 0.1f);
        CHECK (eztempo::detect (tiny.data(), (int) tiny.size(), kSr).bpm == 0.0, "under two seconds of audio has no tempo");
    }
    {
        // leading silence is skipped: the first beat is reported where it is
        const double sec = 16.0 * 60.0 / 120.0;
        auto x = render (120.0, sec, {});
        std::vector<float> padded ((size_t) (kSr * 1.5), 0.0f);
        padded.insert (padded.end(), x.begin(), x.end());
        const auto r = eztempo::detect (padded.data(), (int) padded.size(), kSr, 0.0);
        std::printf ("    padded 120 -> %.2f, first beat %.3f s\n", r.bpm, r.firstBeatSeconds);
        CHECK (near (r.firstBeatSeconds, 1.5, 0.03), "the first beat is found after leading silence");
    }

    std::printf ("\n%d/%d PASS\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
