// ============================================================================
//  sectiontest.cpp -- standalone console test for the section-playback engine
//  additions (Deck.h section loop; Session.h queued seek / armed stop /
//  count-in). Pure C++, no JUCE, no audio device -- proves everything by
//  sample numbers, not by ear, like decktest.cpp and stemplaytest.cpp.
//
//  What a performer needs from these, stated as invariants:
//    - a queued jump fires ON the boundary, never a sample early or late,
//      regardless of the host's block size
//    - a section loop wraps sample-exactly and keeps its sub-sample phase
//    - "play it 3 more times" plays exactly 3 more times
//    - a count-in runs the clock but not the deck
//    - an armed stop freezes the clock at the exact sample and outputs silence
//    - none of it touches a loop-mode deck at all
//
//  Build & run (no CMake needed):
//    cl /nologo /EHsc /std:c++17 /Fe:sectiontest.exe sectiontest.cpp
//    ./sectiontest.exe
// ============================================================================
#include "Session.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int gPass = 0, gTotal = 0;

#define CHECK(cond, desc) do {                                    \
    ++gTotal;                                                     \
    if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); }    \
    else      {          std::printf ("  [FAIL] %s\n", desc); }    \
} while (0)

static bool nearly (double a, double b, double eps = 1e-6) { return std::fabs (a - b) <= eps; }

// A ramp makes the output value equal the playhead position, so "where is the
// deck" can be read straight off the rendered sample.
static std::vector<float> makeRamp (int n)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) i;
    return v;
}

using namespace ezdeck;
using S = Session<2>;

static void loadStem (S& session, int deckIdx, int frames)
{
    auto& d = session.decks[(size_t) deckIdx];
    d.mode = DeckMode::stem;
    d.stemEndBehavior = StemEndBehavior::next;   // stop at the end, don't auto-advance
    d.layers[0].left = makeRamp (frames);
    d.layers[0].loaded = true;
}

struct Out
{
    std::vector<float> l, r;
    std::array<std::vector<float>, kNumLayers> tl, tr;
    std::array<float*, kNumLayers> pl {}, pr {};
    explicit Out (int n) : l ((size_t) n), r ((size_t) n)
    {
        for (int t = 0; t < kNumLayers; ++t)
        {
            tl[(size_t) t].assign ((size_t) n, 0.0f); tr[(size_t) t].assign ((size_t) n, 0.0f);
            pl[(size_t) t] = tl[(size_t) t].data();   pr[(size_t) t] = tr[(size_t) t].data();
        }
    }
};

// Renders `total` samples in blocks of `block`, returns layer-0 output.
static std::vector<float> renderPerTabBlocks (S& session, int total, int block)
{
    std::vector<float> result;
    result.reserve ((size_t) total);
    int done = 0;
    while (done < total)
    {
        const int n = std::min (block, total - done);
        Out out (n);
        session.renderPerTab (out.pl, out.pr, n);
        for (int i = 0; i < n; ++i) result.push_back (out.tl[0][(size_t) i]);
        done += n;
    }
    return result;
}

int main()
{
    std::printf ("Section playback engine tests (Deck.h + Session.h)\n");

    // Tempo 120 BPM 4/4 at 48k => bar = 96000 samples. Keep it small: use
    // 4800 Hz so a bar is 9600 samples and tests run instantly.
    const double sr = 4800.0;
    Tempo tempo; tempo.bpm = 120.0; tempo.beatsPerBar = 4;
    const int64_t bar = barLengthSamples (tempo, sr);   // 9600

    // ------------------------------------------------------------------
    std::printf ("\n-- queued seek: now --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        renderPerTabBlocks (s, 1000, 256);                       // playhead = 1000
        s.queueSeek (5000.0, S::SeekWhen::now);
        auto out = renderPerTabBlocks (s, 10, 10);
        CHECK (nearly (out[0], 5000.0f), "SeekWhen::now lands on the target at the very next sample");
        CHECK (! s.hasPendingSeek(), "a fired seek clears its pending flag");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- queued seek: next bar, independent of block size --\n");
    for (int block : { 1, 7, 64, 1000, 4096 })
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        renderPerTabBlocks (s, 1234, 1000);                      // masterPos 1234, mid-bar
        s.queueSeek (50000.0, S::SeekWhen::nextBar);
        const int toBoundary = (int) (bar - 1234);               // samples until masterPos == bar
        auto out = renderPerTabBlocks (s, toBoundary + 3, block);
        // the sample rendered AT the boundary is the first one after the jump
        const bool before = nearly (out[(size_t) toBoundary - 1], 1234.0f + (float) toBoundary - 1.0f);
        const bool at     = nearly (out[(size_t) toBoundary],     50000.0f);
        char desc[128];
        std::snprintf (desc, sizeof (desc), "block=%d: jump fires exactly on the bar boundary (sample before unchanged, sample at = target)", block);
        CHECK (before && at, desc);
        CHECK (s.masterPosition() == bar + 3, "master clock keeps running through the jump");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- queued seek: end of section (deck position) --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        renderPerTabBlocks (s, 300, 300);                         // playhead 300
        s.queueSeek (20000.0, S::SeekWhen::atDeckPosition, 777.0);   // fire when playhead reaches 777
        auto out = renderPerTabBlocks (s, 600, 128);
        CHECK (nearly (out[476], 776.0f) && nearly (out[477], 20000.0f),
               "atDeckPosition fires when the playhead reaches fireAt (last old sample = 776, next = target)");
    }
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        renderPerTabBlocks (s, 900, 900);
        s.queueSeek (20000.0, S::SeekWhen::atDeckPosition, 777.0);   // already past it
        auto out = renderPerTabBlocks (s, 4, 4);
        CHECK (nearly (out[0], 20000.0f), "a fireAt already behind the playhead fires immediately rather than never");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- cancel --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        s.queueSeek (50000.0, S::SeekWhen::nextBar);
        s.cancelSeek();
        auto out = renderPerTabBlocks (s, (int) bar + 10, 512);
        CHECK (nearly (out[(size_t) bar], (float) bar), "a cancelled seek never fires");
        CHECK (! s.hasPendingSeek(), "cancel clears the pending flag");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- seek target clamped to the stem --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 1000);
        s.switchNow (0);
        s.queueSeek (99999.0, S::SeekWhen::now);
        renderPerTabBlocks (s, 1, 1);
        CHECK (s.decks[0].playheadPosition() <= 1000.0 + 1.0, "a target beyond the stem parks at the stem's end, not past it");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- section loop: sample-exact wrap --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        s.decks[0].setSectionLoop (1000.0, 1400.0);              // 400-sample section
        s.decks[0].seekTo (1000.0);
        auto out = renderPerTabBlocks (s, 1200, 97);             // three passes
        bool ok = true;
        for (int i = 0; i < 1200; ++i)
            if (! nearly (out[(size_t) i], (float) (1000 + (i % 400)))) { ok = false; break; }
        CHECK (ok, "infinite section loop wraps from loopEnd back to loopStart, sample-exact, across odd block sizes");
        CHECK (s.decks[0].isSectionLoopEnabled(), "infinite loop stays armed");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- section loop: n more times --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        s.decks[0].setSectionLoop (1000.0, 1400.0, 3);           // wrap 3 times, then continue
        s.decks[0].seekTo (1000.0);
        auto out = renderPerTabBlocks (s, 400 * 5, 333);
        // passes 1..4 are inside the section (3 wraps), pass 5 continues past 1400
        bool insideOk = true;
        for (int i = 0; i < 400 * 4; ++i)
            if (! nearly (out[(size_t) i], (float) (1000 + (i % 400)))) { insideOk = false; break; }
        const bool continued = nearly (out[400 * 4], 1400.0f) && nearly (out[400 * 4 + 50], 1450.0f);
        CHECK (insideOk, "remaining=3 plays the section 3 extra times");
        CHECK (continued, "...then falls through past loopEnd");
        CHECK (! s.decks[0].isSectionLoopEnabled(), "...and disarms itself");
        CHECK (s.decks[0].getSectionLoopRemaining() == 0, "remaining counts down to 0");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- section loop: phase preserved at rateRatio != 1 --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.decks[0].setRateRatio (1.37);
        s.switchNow (0);
        s.decks[0].setSectionLoop (1000.0, 1400.0);
        s.decks[0].seekTo (1000.0);
        renderPerTabBlocks (s, 5000, 64);
        // after 5000 output samples the deck has advanced 5000*1.37 = 6850 layer
        // samples; modulo the 400-sample section that is 1000 + (6850 mod 400)
        const double expected = 1000.0 + std::fmod (5000.0 * 1.37, 400.0);
        CHECK (nearly (s.decks[0].playheadPosition(), expected, 1e-6),
               "wrapping by loop LENGTH keeps sub-sample phase: no drift after many passes at ratio 1.37");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- section loop: degenerate ranges are inert --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        s.decks[0].setSectionLoop (1400.0, 1000.0);              // end before start
        CHECK (! s.decks[0].isSectionLoopEnabled(), "end <= start never arms");
        s.decks[0].seekTo (1000.0);
        auto out = renderPerTabBlocks (s, 1000, 100);
        CHECK (nearly (out[999], 1999.0f), "...and playback runs straight through");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- count-in --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        s.startCountIn (2 * bar);
        CHECK (s.isCountingIn(), "count-in is armed");
        auto out = renderPerTabBlocks (s, (int) (2 * bar) + 5, 1000);
        bool silent = true;
        for (int i = 0; i < (int) (2 * bar); ++i) if (out[(size_t) i] != 0.0f) { silent = false; break; }
        CHECK (silent, "the deck is silent for the whole count-in");
        CHECK (nearly (out[(size_t) (2 * bar)], 0.0f) && nearly (out[(size_t) (2 * bar) + 4], 4.0f),
               "the deck starts from its own sample 0 the instant the count-in ends");
        CHECK (s.masterPosition() == 2 * bar + 5, "the master clock ran throughout (the metronome has something to count)");
        CHECK (! s.isCountingIn(), "count-in clears itself");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- armed stop --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        s.armStopAt (2500.0);
        auto out = renderPerTabBlocks (s, 3000, 700);
        CHECK (nearly (out[2499], 2499.0f), "plays right up to the stop point");
        bool silentAfter = true;
        for (int i = 2500; i < 3000; ++i) if (out[(size_t) i] != 0.0f) { silentAfter = false; break; }
        CHECK (silentAfter, "...then silence");
        CHECK (s.masterPosition() == 2500, "the clock freezes at the exact stop sample");
        CHECK (s.consumeStopRequest(), "the stop request is raised once");
        CHECK (! s.consumeStopRequest(), "...and only once");
        CHECK (nearly (s.decks[0].playheadPosition(), 2500.0), "the deck is parked exactly where it stopped, ready to resume");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- loop-mode decks are untouched --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        auto& d = s.decks[0];
        d.mode = DeckMode::loop;
        d.layers[0].left = makeRamp (400); d.layers[0].loaded = true;
        s.switchNow (0);
        d.setSectionLoop (100.0, 200.0);
        s.queueSeek (300.0, S::SeekWhen::now);
        s.armStopAt (150.0);
        auto out = renderPerTabBlocks (s, 800, 128);
        bool plainLoop = true;
        for (int i = 0; i < 800; ++i) if (! nearly (out[(size_t) i], (float) (i % 400))) { plainLoop = false; break; }
        CHECK (plainLoop, "a loop-mode deck loops its region exactly as before -- no seek, no section loop, no stop");
        CHECK (s.hasPendingSeek(), "the seek stays pending rather than being consumed by a loop deck");
        CHECK (! s.consumeStopRequest(), "no stop request from a loop deck");
    }

    // ------------------------------------------------------------------
    std::printf ("\n-- legacy render() path honours the same controls --\n");
    {
        S s; s.prepare (sr); s.setTempo (tempo);
        loadStem (s, 0, 100000);
        s.switchNow (0);
        s.armStopAt (1000.0);
        std::vector<float> l (2000), r (2000);
        s.render (l.data(), r.data(), 2000, 1.0f);
        CHECK (s.masterPosition() == 1000 && s.consumeStopRequest(), "render() stops at the same sample renderPerTab() would");
    }

    std::printf ("\n%d / %d passed\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
