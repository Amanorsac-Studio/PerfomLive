// ============================================================================
//  tempochangetest.cpp -- the live tempo change (Session::scheduleTempoChange).
//
//  Owner: "while a loop or stems are playing I press plus and the tempo goes
//  up in the next bar." Proven by sample numbers: a stem whose samples are
//  their own musical position (a ramp) shows exactly where the song is, so
//  "the change landed on the bar, and the song carried on from the same
//  place at the new speed" can be read straight off the output.
//
//    cl /nologo /EHsc /std:c++17 tempochangetest.cpp && tempochangetest.exe
// ============================================================================
#include "Session.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int gPass = 0, gTotal = 0;
#define CHECK(cond, desc) do { ++gTotal; if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); } \
                               else std::printf ("  [FAIL] %s\n", desc); } while (0)

using namespace ezdeck;
using S = Session<2>;

namespace
{
    constexpr double kSr = 48000.0;
    constexpr int64_t kBar120 = 96000;    // 4/4 at 120 BPM
    constexpr int64_t kBar100 = 115200;   // 4/4 at 100 BPM

    // a stem whose sample i holds its musical position (in 120 BPM samples)
    std::vector<float> positions (int frames, double scale)
    {
        std::vector<float> v ((size_t) frames);
        for (int i = 0; i < frames; ++i) v[(size_t) i] = (float) ((double) i / scale);
        return v;
    }

    void setUp (S& s)
    {
        s.prepare (kSr);
        s.setTempo ({ 120.0, 4 });
        auto& d = s.decks[0];
        d.mode = DeckMode::stem;
        d.stemEndBehavior = StemEndBehavior::next;
        d.layers[0].left = positions (1200000, 1.0);
        d.layers[0].loaded = true;
        s.switchNow (0);
    }

    // stages the song re-stretched by `scale` and asks for the change
    void change (S& s, double bpm, double scale, int frames)
    {
        s.decks[0].layers[0].stagePendingSwap (positions (frames, scale), {}, 0);
        s.scheduleTempoChange (bpm, scale);
    }

    std::vector<float> play (S& s, int samples, int block)
    {
        std::vector<float> out;
        std::vector<float> l ((size_t) block), r ((size_t) block);
        for (int done = 0; done < samples; done += block)
        {
            const int n = std::min (block, samples - done);
            s.render (l.data(), r.data(), n, 1.0f);
            out.insert (out.end(), l.begin(), l.begin() + n);
        }
        return out;
    }

    // the first sample (after `from`) where the song stops moving one-for-one
    int64_t firstSlowdown (const std::vector<float>& v, int64_t from)
    {
        for (size_t i = (size_t) from + 1; i < v.size(); ++i)
            if (std::fabs ((v[i] - v[i - 1]) - 1.0f) > 0.01f) return (int64_t) i - 1;   // sample i-1 is the first at the new speed
        return -1;
    }
}

int main()
{
    std::printf ("live tempo change (Session::scheduleTempoChange)\n");

    for (int block : { 512, 97 })
    {
        S s;
        setUp (s);
        auto before = play (s, 100000, block);        // into bar 2
        change (s, 100.0, 1.2, 1440000);               // 120 -> 100 BPM: 20% longer
        auto after = play (s, 250000, block);
        std::vector<float> all = before;
        all.insert (all.end(), after.begin(), after.end());

        const int64_t at = firstSlowdown (all, 0);
        std::printf ("    block %d: change heard at sample %lld\n", block, (long long) at);
        if (block == 512)
        {
            CHECK (at == 2 * kBar120, "the tempo change lands exactly on the next bar line (bar 3)");
            CHECK (std::fabs (all[(size_t) at] - (float) (2 * kBar120)) < 0.01f,
                   "the song carries on from the same musical place, not from the top");
            CHECK (std::fabs ((all[(size_t) at + 1000] - all[(size_t) at]) - 1000.0f / 1.2f) < 0.05f,
                   "after the change the song moves at the new speed (100/120)");
            CHECK (s.tempoChangesApplied() == 1 && ! s.isTempoChangePending() && s.getTempo().bpm == 100.0,
                   "the new tempo is in, and the change is counted once");
            CHECK (s.barOriginPosition() == 2 * kBar120, "bars now count from the change");
        }
        else
        {
            CHECK (at == 2 * kBar120, "the same bar line with a different block size (97 samples)");
        }
    }

    {
        // the next change lands one 100-BPM bar after the first, not on the old grid
        S s;
        setUp (s);
        play (s, 100000, 512);
        change (s, 100.0, 1.2, 1440000);
        play (s, 100000, 512);                                  // past the first change (at 192000)
        s.decks[0].layers[0].stagePendingSwap (positions (1200000, 1.0), {}, 0);
        s.scheduleTempoChange (120.0, 1.0 / 1.2);
        std::vector<float> seen = play (s, 200000, 512);        // from 200000
        int64_t back = -1;
        for (size_t i = 1; i < seen.size(); ++i)
            if (std::fabs ((seen[i] - seen[i - 1]) - 1.0f) < 0.01f) { back = 200000 + (int64_t) i - 1; break; }
        std::printf ("    second change heard at %lld (expected %lld)\n", (long long) back, (long long) (2 * kBar120 + kBar100));
        CHECK (back == 2 * kBar120 + kBar100, "a second change lands on the bar line of the new tempo (bar 1 at 100 BPM later)");
        CHECK (s.getTempo().bpm == 120.0 && s.tempoChangesApplied() == 2, "and brings the tempo back");
    }

    {
        // a queued jump moves with the song
        S s;
        setUp (s);
        play (s, 10000, 512);
        s.queueSeek (500000.0, S::SeekWhen::atDeckPosition, 400000.0);
        change (s, 100.0, 1.2, 1440000);
        play (s, 200000, 512);   // the change applies at 96000
        CHECK (std::fabs (s.pendingSeekTarget() - 600000.0) < 0.5, "a jump waiting to happen is moved to the same musical place");
    }

    {
        S s;
        setUp (s);
        play (s, 100000, 512);
        change (s, 100.0, 1.2, 1440000);
        play (s, 150000, 512);
        s.resetTransport();
        CHECK (s.barOriginPosition() == 0 && ! s.isTempoChangePending(), "starting again counts bars from zero");
        s.scheduleTempoChange (90.0, 1.0);
        s.switchNow (0);
        CHECK (! s.isTempoChangePending(), "starting a row drops a change that was still waiting");
    }

    std::printf ("\n%d/%d PASS\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
