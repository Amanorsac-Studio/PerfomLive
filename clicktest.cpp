// ============================================================================
//  clicktest.cpp -- standalone checks for ClickEngine.h (no JUCE).
//    g++ -std=c++17 -O2 clicktest.cpp -o clicktest && ./clicktest
// ============================================================================
#include "ClickEngine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int gPass = 0, gTotal = 0;
#define CHECK(cond, desc) do { ++gTotal; if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); } \
                               else std::printf ("  [FAIL] %s\n", desc); } while (0)

using namespace ezclick;

namespace
{
    constexpr double kSr = 48000.0;
    constexpr double kBeat = 24000.0;   // 120 BPM at 48 kHz

    struct Take { std::vector<float> l, r; };

    Take play (Clicker& c, double seconds, double startPos = 0.0, int beatsPerBar = 4, int block = 512)
    {
        Take t;
        const int n = (int) (seconds * kSr);
        t.l.assign ((size_t) n, 0.0f);
        t.r.assign ((size_t) n, 0.0f);
        for (int at = 0; at < n; at += block)
        {
            const int len = std::min (block, n - at);
            c.render (t.l.data() + at, t.r.data() + at, len, startPos + at, kBeat, beatsPerBar);
        }
        return t;
    }

    float peakIn (const std::vector<float>& x, size_t from, size_t to)
    {
        float p = 0.0f;
        for (size_t i = from; i < std::min (to, x.size()); ++i) p = std::max (p, std::fabs (x[i]));
        return p;
    }

    // how many separate clicks start in the take (a start = sound after >= 5 ms of silence)
    int countStrikes (const std::vector<float>& x)
    {
        int strikes = 0;
        size_t quiet = 100000;
        for (float v : x)
        {
            if (std::fabs (v) > 1.0e-4f) { if (quiet >= (size_t) (0.005 * kSr)) ++strikes; quiet = 0; }
            else ++quiet;
        }
        return strikes;
    }
}

int main()
{
    std::printf ("ClickEngine.h\n");
    SoundBank bank;
    bank.build (kSr);

    {
        ClickParams p;
        Clicker c (bank, p);
        const auto t = play (c, 2.0);   // one bar of 4/4 at 120
        CHECK (countStrikes (t.l) == 4, "one click on each beat of a bar");
        const float one = peakIn (t.l, 0, 2000), two = peakIn (t.l, (size_t) kBeat, (size_t) kBeat + 2000);
        CHECK (one > 0.7f && std::fabs (two - 0.62f * 0.8f) < 0.02f, "beat one is at full level, the others at the beat level");
        CHECK (t.l == t.r, "the click is the same in both ears");
    }
    {
        ClickParams p;
        p.subdivision = 2;
        Clicker c (bank, p);
        const auto t = play (c, 2.0);
        CHECK (countStrikes (t.l) == 8, "eighths: a click on every beat and every half beat");
        const float andLevel = peakIn (t.l, (size_t) (kBeat * 0.5), (size_t) (kBeat * 0.5) + 2000);
        CHECK (std::fabs (andLevel - 0.35f * 0.8f) < 0.02f, "the half-beat clicks play at the subdivision level");
        p.subdivision = 3;
        c.reset();
        CHECK (countStrikes (play (c, 2.0).l) == 12, "triplets: three clicks per beat");
        p.subdivision = 4;
        c.reset();
        CHECK (countStrikes (play (c, 2.0).l) == 16, "sixteenths: four clicks per beat");
    }
    {
        ClickParams p;
        p.accent = false;
        Clicker c (bank, p);
        const auto t = play (c, 2.0);
        const float one = peakIn (t.l, 0, 2000), two = peakIn (t.l, (size_t) kBeat, (size_t) kBeat + 2000);
        CHECK (std::fabs (one - two) < 0.01f, "with accent off, beat one is like every other beat");
    }
    {
        ClickParams p;
        p.level = 0.5f;
        Clicker c (bank, p);
        const auto t = play (c, 0.2);
        CHECK (std::fabs (peakIn (t.l, 0, 2000) - 0.4f) < 0.02f, "the click level scales the whole click");
        p.beatLevel = 0.0f;
        c.reset();
        CHECK (countStrikes (play (c, 2.0).l) == 1, "a beat level of zero leaves only beat one");
    }
    {
        ClickParams p;
        Clicker c (bank, p);
        const auto t = play (c, 2.0, 0.0, 3);   // 3/4
        const float bar2 = peakIn (t.l, (size_t) (kBeat * 3), (size_t) (kBeat * 3) + 2000);
        CHECK (bar2 > 0.7f, "in 3/4 the accent comes back on the fourth beat (the next bar's one)");
    }
    {
        // counting in: a position before zero still lands on beats, beat one on bar lines
        ClickParams p;
        Clicker c (bank, p);
        const auto t = play (c, 2.0, -2.0 * kBeat * 2.0);   // two bars before the song, 4/4
        CHECK (countStrikes (t.l) == 4 && peakIn (t.l, 0, 2000) > 0.7f, "a count-in (negative positions) clicks on the beat, accented on its bar line");
    }
    {
        // every sound is different, and accent/beat/sub differ within a sound
        bool allDifferent = true, voicesDiffer = true, allAudible = true;
        for (int s = 0; s < kNumSounds; ++s)
        {
            const auto& a = bank.get ((Sound) s, Hit::accent);
            const auto& b = bank.get ((Sound) s, Hit::beat);
            if (a == b) voicesDiffer = false;
            if (peakIn (a, 0, a.size()) < 0.5f || peakIn (b, 0, b.size()) < 0.5f) allAudible = false;
            for (int o = s + 1; o < kNumSounds; ++o)
                if (bank.get ((Sound) o, Hit::beat) == b) allDifferent = false;
        }
        CHECK (allDifferent, "each of the seven sounds is its own sound");
        CHECK (voicesDiffer, "every sound has a distinct beat-one voice");
        CHECK (allAudible, "every sound is rendered at a clear level");
        CHECK (std::string (soundName (Sound::woodblock)) == "Woodblock" && std::string (soundName (Sound::beep)) == "Beep",
               "sounds have names to show");
    }
    {
        ClickParams p;
        p.sound = 99;   // a stored value from nowhere
        Clicker c (bank, p);
        const auto t = play (c, 0.5);
        CHECK (countStrikes (t.l) == 1, "an unknown sound number still clicks (the last sound), never reads out of range");
    }
    {
        ClickParams p;
        Clicker c (bank, p);
        std::vector<float> l (512, 1.0f), r (512, 1.0f);
        c.render (l.data(), r.data(), 512, 0.0, 0.0, 4);
        bool silent = true;
        for (float v : l) if (v != 0.0f) silent = false;
        CHECK (silent, "no tempo: the buffer is cleared and nothing clicks");
    }

    std::printf ("\n%d/%d PASS\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
