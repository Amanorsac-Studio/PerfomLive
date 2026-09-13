// ============================================================================
//  switchtest.cpp — standalone console test for Session.h's bar-quantized
//  deck switching.
//
//  Pure C++, no JUCE, no audio device. Proves the switch lands exactly on
//  the bar boundary by numbers:
//    - the boundary/bar-length math is correct at the edges (pos already on
//      a boundary, pos one sample past one, pos zero)
//    - a trigger fired mid-bar produces a switch at the exact computed
//      sample, regardless of how the render() calls are chunked into blocks
//    - the new deck's layer phase is exactly 0 at the first sample it plays
//      (phase-aligned to bar zero)
//    - queuing the already-active deck is a no-op
//
//  Build & run (no CMake needed — this file has zero JUCE dependency):
//    g++ -std=c++17 -O2 switchtest.cpp -o switchtest.exe
//    ./switchtest.exe
// ============================================================================
#include "Session.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static int gPass = 0, gTotal = 0;

#define CHECK(cond, desc) do {                                    \
    ++gTotal;                                                     \
    if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); }    \
    else      {          std::printf ("  [FAIL] %s\n", desc); }    \
} while (0)

static std::vector<float> makeRamp (int n)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) i;
    return v;
}

static bool nearlyEqual (double a, double b, double eps = 1e-9)
{
    return std::fabs (a - b) <= eps;
}

int main()
{
    using namespace ezdeck;

    std::printf ("Session.h bar-quantized switch tests\n");

    // ---- pure boundary math edge cases -------------------------------------
    {
        CHECK (nextBarBoundaryAtOrAfter (0, 1000) == 0,
               "position already at 0 is its own boundary");
        CHECK (nextBarBoundaryAtOrAfter (1000, 1000) == 1000,
               "position exactly on a boundary returns itself (immediate switch is valid)");
        CHECK (nextBarBoundaryAtOrAfter (1001, 1000) == 2000,
               "one sample past a boundary rounds up to the NEXT full bar, not the one just passed");
        CHECK (nextBarBoundaryAtOrAfter (999, 1000) == 1000,
               "one sample before a boundary rounds up to that boundary");

        Tempo t { 120.0, 4 };
        CHECK (barLengthSamples (t, 44100.0) == 88200,
               "120 BPM, 4/4, 44.1kHz -> exactly 2.0s bars (88200 samples), no rounding drift");
    }

    // ---- trigger mid-bar: switch lands on the exact sample, any chunking ---
    //
    // Deck A and deck B are given numerically distinguishable content (0..999
    // vs 10000..10999), so which deck produced a given output sample can be
    // read directly off the data — no reliance on querying activeDeck() at
    // arbitrary points, which would only reflect state at the END of whatever
    // block happened to be rendered (and say nothing about samples that were
    // rendered and switched mid-block).
    {
        const int barLen = 1000;   // 120 BPM, 4/4 @ chosen rate gives a clean 1000-sample bar
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);

        auto rampA = makeRamp (barLen);              // values 0..999
        std::vector<float> rampB (barLen);
        for (int i = 0; i < barLen; ++i) rampB[(size_t) i] = 10000.0f + (float) i;   // 10000..10999

        auto runOnce = [&] (int chunkSize)
        {
            Session<2> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].layers[0].left = rampA; s.decks[0].layers[0].loaded = true;
            s.decks[1].layers[0].left = rampB; s.decks[1].layers[0].loaded = true;

            const int total  = barLen * 3 + 250;   // trigger mid-bar, run past two more bars
            const int fireAt = barLen + 400;       // fixed absolute sample, independent of chunking
            std::vector<float> L (total), R (total);

            bool fired = false;
            int done = 0;
            while (done < total)
            {
                int chunk = std::min (chunkSize, total - done);
                if (! fired) chunk = std::min (chunk, fireAt - done);   // don't cross the fire point in one call

                s.render (L.data() + done, R.data() + done, chunk, 1.0f);
                done += chunk;

                if (! fired && done >= fireAt)
                {
                    s.queueSwitch (1);
                    fired = true;
                }
            }
            return L;
        };

        auto bigChunks   = runOnce (10'000);   // one huge block
        auto smallChunks = runOnce (37);       // many small, uneven blocks

        CHECK (bigChunks == smallChunks,
               "output samples are bit-identical regardless of how render() calls are chunked into blocks");

        CHECK (bigChunks[(size_t) (2 * barLen - 1)] < 1000.0f,
               "the sample just before the boundary still belongs to deck A");
        CHECK (bigChunks[(size_t) (2 * barLen)] == 10000.0f,
               "the very next sample is deck B's OWN sample 0 — switch is sample-exact AND phase-aligned to bar zero");

        bool aUnaffectedUntilSwitch = true;
        for (int i = 0; i < 2 * barLen; ++i)
            if (std::fabs (bigChunks[(size_t) i] - rampA[(size_t) (i % barLen)]) > 1e-6f)
                { aUnaffectedUntilSwitch = false; break; }
        CHECK (aUnaffectedUntilSwitch,
               "deck A keeps playing its own uninterrupted loop through both bars after being triggered, "
               "with no glitch from the pending switch");
    }

    // ---- new deck starts phase-aligned to bar zero -------------------------
    {
        const int barLen = 500;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].layers[0].left = makeRamp (barLen);
        s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = makeRamp (barLen);
        s.decks[1].layers[0].loaded = true;

        s.queueSwitch (1);   // fire immediately, at masterPosition() == 0

        std::vector<float> L (barLen), R (barLen);
        s.render (L.data(), R.data(), barLen);

        CHECK (s.activeDeck() == 1, "queuing at position 0 switches immediately (0 is a valid boundary)");
        CHECK (s.decks[1].phaseOf (0) == std::fmod ((double) barLen, (double) barLen),
               "deck B's layer phase after exactly one bar equals a fresh reset()'s trajectory");

        Session<1> fresh;
        fresh.prepare (sr);
        fresh.decks[0].layers[0].left = makeRamp (barLen);
        fresh.decks[0].layers[0].loaded = true;
        std::vector<float> fL (barLen), fR (barLen);
        fresh.render (fL.data(), fR.data(), barLen);

        CHECK (L == fL && R == fR,
               "deck B's output after switching matches a deck that started at playhead 0 from sample 0 "
               "(proves the new deck was reset, not resumed mid-buffer)");
    }

    // ---- queuing the already-active deck is a no-op ------------------------
    {
        Session<2> s;
        s.setTempo ({ 120.0, 4 });
        s.prepare (44100.0);
        CHECK (s.activeDeck() == 0, "deck 0 is active by default");
        s.queueSwitch (0);
        CHECK (s.queuedDeck() == -1, "queuing the deck that's already active does not queue a switch");
    }

    // ---- M4-T7: scheduled re-warp applies at the exact bar boundary, any chunking ----
    //
    // Mirrors the deck-switch test above exactly: old and new content are
    // numerically distinguishable, so which buffer produced a given output
    // sample can be read directly off the data.
    {
        const int barLen = 1000;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);

        auto oldRamp = makeRamp (barLen);              // values 0..999
        std::vector<float> newRamp (barLen);
        for (int i = 0; i < barLen; ++i) newRamp[(size_t) i] = 10000.0f + (float) i;   // 10000..10999

        auto runOnce = [&] (int chunkSize)
        {
            Session<1> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].layers[0].left = oldRamp; s.decks[0].layers[0].loaded = true;

            const int total  = barLen * 3 + 250;
            const int fireAt = barLen + 400;
            std::vector<float> L (total), R (total);

            bool fired = false;
            int done = 0;
            while (done < total)
            {
                int chunk = std::min (chunkSize, total - done);
                if (! fired) chunk = std::min (chunk, fireAt - done);

                s.render (L.data() + done, R.data() + done, chunk, 1.0f);
                done += chunk;

                if (! fired && done >= fireAt)
                {
                    s.decks[0].layers[0].stagePendingSwap (newRamp, {}, barLen);
                    s.scheduleActiveDeckRewarp();
                    fired = true;
                }
            }
            return L;
        };

        auto bigChunks   = runOnce (10'000);
        auto smallChunks = runOnce (37);

        CHECK (bigChunks == smallChunks,
               "re-warp application is bit-identical regardless of how render() calls are chunked into blocks");

        CHECK (bigChunks[(size_t) (2 * barLen - 1)] < 1000.0f,
               "the sample just before the re-warp boundary still reflects the OLD buffer");
        CHECK (bigChunks[(size_t) (2 * barLen)] == 10000.0f,
               "the very next sample is the NEW buffer's OWN sample 0 — re-warp is sample-exact AND phase-aligned to bar zero");

        bool oldUnaffectedUntilRewarp = true;
        for (int i = 0; i < 2 * barLen; ++i)
            if (std::fabs (bigChunks[(size_t) i] - oldRamp[(size_t) (i % barLen)]) > 1e-6f)
                { oldUnaffectedUntilRewarp = false; break; }
        CHECK (oldUnaffectedUntilRewarp,
               "the deck keeps playing its OLD buffer uninterrupted through both bars after being scheduled, "
               "with no glitch from the pending re-warp");
    }

    // ---- M4-T7: a queued switch and a pending re-warp at the SAME boundary — switch wins ----
    {
        const int barLen = 500;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        auto oldDeck0 = makeRamp (barLen);
        s.decks[0].layers[0].left = oldDeck0; s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = makeRamp (barLen); s.decks[1].layers[0].loaded = true;

        auto newDeck0Content = makeRamp (barLen);
        for (auto& v : newDeck0Content) v += 5000.0f;   // distinguishable from anything else in this test
        s.decks[0].layers[0].stagePendingSwap (newDeck0Content, {}, barLen);
        s.scheduleActiveDeckRewarp();   // scheduled against deck 0, currently active
        s.queueSwitch (1);              // ALSO queue a switch to deck 1, same upcoming boundary

        std::vector<float> L (barLen), R (barLen);
        s.render (L.data(), R.data(), barLen);   // the boundary is reached exactly at the end of this one bar

        CHECK (s.activeDeck() == 1,
               "a queued switch takes effect even when a re-warp was scheduled for the same boundary");
        CHECK (s.decks[0].layers[0].pendingSwapReady.load(),
               "deck 0's re-warp scheduled for that same boundary was never applied — the switch took precedence");
        CHECK (s.decks[0].layers[0].left == oldDeck0,
               "deck 0's audio content is untouched — its staged swap was not silently applied while it became inactive");

        // Switch back to deck 0 WITHOUT re-scheduling a re-warp -- its stale
        // staged swap must stay inert. scheduleActiveDeckRewarp() is the
        // only thing that ever applies it, never an implicit side effect of
        // a deck becoming active again (this is exactly the misfire
        // ARCHITECTURE.md's resolved Decision #2 calls out by name).
        s.queueSwitch (0);
        std::vector<float> L2 (barLen * 2), R2 (barLen * 2);
        s.render (L2.data(), R2.data(), barLen * 2);

        CHECK (s.activeDeck() == 0, "switching back to deck 0 succeeds");
        CHECK (s.decks[0].layers[0].pendingSwapReady.load() && s.decks[0].layers[0].left == oldDeck0,
               "deck 0's stale staged swap remains inert across further bars unless scheduleActiveDeckRewarp() is called again");
    }

    // ---- M5-T3: nextPlay auto-queues immediately, but the switch itself stays bar-quantized ----
    {
        const int barLen = 1000;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);
        const int stemLen = 400;

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].mode = DeckMode::stem;   // stemEndBehavior defaults to nextPlay
        s.decks[0].layers[0].left = makeRamp (stemLen);
        s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = makeRamp (barLen);
        s.decks[1].layers[0].loaded = true;

        std::vector<float> L (stemLen + 50), R (stemLen + 50);
        s.render (L.data(), R.data(), stemLen + 50, 1.0f);

        CHECK (s.activeDeck() == 0,
               "the deck stays active immediately after its stem finishes -- the actual switch is still bar-quantized");
        CHECK (s.queuedDeck() == 1,
               "nextPlay auto-queues the only other deck with content as soon as the stem finishes");
    }

    // ---- M5-T3: the auto-triggered switch is sample-exact, phase-aligned, and block-size independent ----
    {
        const int barLen  = 1000;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);
        const int stemLen = 400;

        auto stemContent = makeRamp (stemLen);
        std::vector<float> deck1Content (barLen);
        for (int i = 0; i < barLen; ++i) deck1Content[(size_t) i] = 10000.0f + (float) i;

        auto runOnce = [&] (int chunkSize)
        {
            Session<2> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].mode = DeckMode::stem;
            s.decks[0].layers[0].left = stemContent;
            s.decks[0].layers[0].loaded = true;
            s.decks[1].layers[0].left = deck1Content;
            s.decks[1].layers[0].loaded = true;

            const int total = barLen + 250;
            std::vector<float> L (total), R (total);
            int done = 0;
            while (done < total)
            {
                const int chunk = std::min (chunkSize, total - done);
                s.render (L.data() + done, R.data() + done, chunk, 1.0f);
                done += chunk;
            }
            return L;
        };

        auto bigChunks   = runOnce (10'000);
        auto smallChunks = runOnce (37);

        CHECK (bigChunks == smallChunks,
               "nextPlay's auto-triggered switch is bit-identical regardless of how render() calls are chunked into blocks");
        CHECK (bigChunks[(size_t) (barLen - 1)] == 0.0f,
               "the sample just before the boundary is deck 0's silence (its stem already finished well before the bar line)");
        CHECK (bigChunks[(size_t) barLen] == 10000.0f,
               "the very next sample is deck 1's own sample 0 -- the auto-triggered switch is sample-exact AND phase-aligned to bar zero");
    }

    // ---- M5-T3: nextPlay is a no-op when no other deck has any content ------
    {
        Session<2> s;
        s.setTempo ({ 120.0, 4 });
        s.prepare (44100.0);
        s.decks[0].mode = DeckMode::stem;
        s.decks[0].layers[0].left = makeRamp (100);
        s.decks[0].layers[0].loaded = true;
        // deck 1 has nothing loaded at all

        std::vector<float> L (500), R (500);
        s.render (L.data(), R.data(), 500, 1.0f);

        CHECK (s.queuedDeck() == -1, "nextPlay with no other deck loaded queues nothing");
        CHECK (s.activeDeck() == 0, "the deck stays active (silent) since there's nowhere to advance to");
    }

    // ---- M5-T3: loop end-behavior resets EXACTLY at the stem's own end, even in one large render() call ----
    {
        Session<2> s;
        s.setTempo ({ 120.0, 4 });
        s.prepare (44100.0);
        const int stemLen = 100;
        s.decks[0].mode = DeckMode::stem;
        s.decks[0].stemEndBehavior = StemEndBehavior::loop;
        s.decks[0].layers[0].left = makeRamp (stemLen);
        s.decks[0].layers[0].loaded = true;

        // one call, deliberately much larger than stemLen -- this is exactly
        // the scenario that would silently delay the reset (and cause an
        // audible gap) without the stem-end chunk clamp in Session::render().
        std::vector<float> L (5000), R (5000);
        s.render (L.data(), R.data(), 5000, 1.0f);

        CHECK (s.activeDeck() == 0, "loop end-behavior never switches decks");
        CHECK (s.queuedDeck() == -1, "loop end-behavior never queues a switch");
        // after looping 5000/100 = 50 full cycles, playhead should be back
        // near a small residual, never allowed to run past stemLen unchecked
        CHECK (s.decks[0].playheadPosition() < (double) stemLen,
               "the deck's playhead never overruns stemLength() by more than one reset cycle, even across many resets in one render() call");
    }

    // ---- M5-T3: next end-behavior silences without switching or resetting --
    {
        Session<2> s;
        s.setTempo ({ 120.0, 4 });
        s.prepare (44100.0);
        const int stemLen = 100;
        s.decks[0].mode = DeckMode::stem;
        s.decks[0].stemEndBehavior = StemEndBehavior::next;
        s.decks[0].layers[0].left = makeRamp (stemLen);
        s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = makeRamp (500);
        s.decks[1].layers[0].loaded = true;   // deck 1 has content, but "next" must not advance to it

        std::vector<float> L (stemLen + 200), R (stemLen + 200);
        s.render (L.data(), R.data(), stemLen + 200, 1.0f);

        CHECK (s.activeDeck() == 0, "next end-behavior never switches decks, even when another deck has content");
        CHECK (s.queuedDeck() == -1, "next end-behavior never queues a switch");

        bool silentPastEnd = true;
        for (int i = stemLen; i < stemLen + 200; ++i)
            if (L[(size_t) i] != 0.0f) { silentPastEnd = false; break; }
        CHECK (silentPastEnd, "next end-behavior leaves the deck silent past its stem's end, indefinitely");
    }

    // ---- M5-T3: nextPlay never clobbers an explicit, already-queued manual switch ----
    // Uses 3 decks specifically so nextPlay's own natural target (deck 1,
    // the next one after deck 0 with content) differs from the deck the
    // user explicitly requested (deck 2) -- if the guard were missing, the
    // auto-queue would silently overwrite queued back to 1.
    {
        const int barLen = 1000;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);
        const int stemLen = 100;

        Session<3> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].mode = DeckMode::stem;   // nextPlay (default)
        s.decks[0].layers[0].left = makeRamp (stemLen);
        s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = makeRamp (500);
        s.decks[1].layers[0].loaded = true;   // nextPlay's own natural target
        s.decks[2].layers[0].left = makeRamp (500);
        s.decks[2].layers[0].loaded = true;   // the deck the user explicitly wants instead

        // Render a few samples first so masterPos moves off the trivial
        // boundary at 0 -- queueSwitch()'d exactly at masterPos 0 would
        // resolve immediately (0 is itself a valid boundary), which would
        // defeat this test before it ever reaches the stem's own end.
        std::vector<float> L0 (10), R0 (10);
        s.render (L0.data(), R0.data(), 10, 1.0f);

        s.queueSwitch (2);   // explicit, user-requested switch, queued mid-bar, well before the stem finishes
        CHECK (s.queuedDeck() == 2, "sanity: the explicit queueSwitch(2) took effect and is still pending");

        // render past the stem's own end (100, i.e. 90 more samples from
        // here) but nowhere near the bar boundary (1000)
        std::vector<float> L (200), R (200);
        s.render (L.data(), R.data(), 200, 1.0f);

        CHECK (s.queuedDeck() == 2,
               "nextPlay's own auto-queue logic does not overwrite an already-queued explicit switch to a DIFFERENT deck");
    }

    // ---- M5-T3: findNextDeckWithContent() skips an empty deck and wraps ----
    {
        Session<3> s;
        s.setTempo ({ 120.0, 4 });
        s.prepare (44100.0);
        // deck 0 active (default), deck 1 empty, deck 2 has content
        s.decks[2].layers[0].left = makeRamp (100);
        s.decks[2].layers[0].loaded = true;

        CHECK (s.findNextDeckWithContent() == 2,
               "findNextDeckWithContent() skips an empty deck 1 and finds deck 2 further around the sequence");
    }

    // ---- M5-T4: stem-to-stem crossfade -- exact gain ramp, exact duration, exact boundary ----
    // sr=1000Hz with 120 BPM/4-4 gives a clean barLen of 2000 samples and a
    // clean crossfade duration of 120 samples (0.120s * 1000Hz), so every
    // expected value below is an exact, hand-computable number.
    {
        Tempo t { 120.0, 4 };
        const double sr = 1000.0;
        const int crossfadeLen = 120;

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].mode = DeckMode::stem;
        s.decks[1].mode = DeckMode::stem;
        s.decks[0].layers[0].left = std::vector<float> (5000, 1.0f);   // constant, distinguishable
        s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = std::vector<float> (5000, 2.0f);
        s.decks[1].layers[0].loaded = true;

        s.queueSwitch (1);   // masterPos is 0, a trivial boundary -- the crossfade starts immediately

        std::vector<float> L (crossfadeLen + 50), R (crossfadeLen + 50);
        s.render (L.data(), R.data(), (int) L.size(), 0.5f);   // masterGain 0.5 -> deck0*0.5=0.5, deck1*0.5=1.0 at full volume

        CHECK (nearlyEqual ((double) L[0], 0.5, 1e-4),
               "crossfade sample 0: fully the outgoing deck's own gain-scaled content (fromGain=1, toGain=0)");
        CHECK (nearlyEqual ((double) L[60], 0.75, 1e-3),
               "crossfade sample 60 (halfway through 120): both decks contribute proportionally to the linear ramp");

        const double expectedLast = (1.0 / 120.0) * 0.5 + (119.0 / 120.0) * 1.0;
        CHECK (nearlyEqual ((double) L[(size_t) (crossfadeLen - 1)], expectedLast, 1e-3),
               "crossfade's last sample (119) matches the expected linear-ramp value -- not yet fully at the destination");
        CHECK (nearlyEqual ((double) L[(size_t) crossfadeLen], 1.0, 1e-4),
               "the sample immediately after the crossfade concludes is the destination deck's own full-volume output");

        CHECK (s.activeDeck() == 1, "activeDeck() flips to the destination deck exactly once the crossfade concludes");
    }

    // ---- M5-T4: activeDeck() vs isDeckAudible() during the crossfade window ----
    {
        Tempo t { 120.0, 4 };
        const double sr = 1000.0;

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].mode = DeckMode::stem;
        s.decks[1].mode = DeckMode::stem;
        s.decks[0].layers[0].left = std::vector<float> (500, 1.0f); s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = std::vector<float> (500, 1.0f); s.decks[1].layers[0].loaded = true;

        CHECK (s.isDeckAudible (0) && ! s.isDeckAudible (1),
               "before any switch, only deck 0 (active) is audible");

        s.queueSwitch (1);
        std::vector<float> L (60), R (60);   // halfway through the 120-sample crossfade
        s.render (L.data(), R.data(), 60, 1.0f);

        CHECK (s.activeDeck() == 0, "activeDeck() still names the outgoing deck mid-crossfade (Model B)");
        CHECK (s.isDeckAudible (0) && s.isDeckAudible (1),
               "BOTH decks are audible mid-crossfade, even though activeDeck() only names one of them");

        std::vector<float> L2 (100), R2 (100);
        s.render (L2.data(), R2.data(), 100, 1.0f);   // finishes the crossfade

        CHECK (s.activeDeck() == 1 && s.isDeckAudible (1) && ! s.isDeckAudible (0),
               "after the crossfade concludes, activeDeck() and isDeckAudible() agree again");
    }

    // ---- M5-T4: a switch involving a loop-mode deck still hard-cuts, unmodified ----
    {
        const int barLen = 1000;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].mode = DeckMode::stem;   // deck 1 left at default loop mode -- not a stem-to-stem handoff
        auto rampA = makeRamp (barLen);
        std::vector<float> rampB (barLen);
        for (int i = 0; i < barLen; ++i) rampB[(size_t) i] = 10000.0f + (float) i;
        s.decks[0].layers[0].left = rampA; s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = rampB; s.decks[1].layers[0].loaded = true;

        // Render a few samples BEFORE queuing the switch, so masterPos
        // moves off the trivial boundary at 0 -- queueSwitch()'d exactly at
        // masterPos 0 would resolve immediately (0 is itself a valid
        // boundary), defeating this test before it ever reaches barLen.
        std::vector<float> L0 (10), R0 (10);
        s.render (L0.data(), R0.data(), 10, 1.0f);

        s.queueSwitch (1);
        std::vector<float> L (barLen - 10 + 5), R (barLen - 10 + 5);
        s.render (L.data(), R.data(), (int) L.size(), 1.0f);

        CHECK (s.activeDeck() == 1, "a stem-mode-to-loop-mode switch still switches (unmodified hard cut)");
        const size_t boundaryIdx = (size_t) (barLen - 10);   // index within L corresponding to absolute sample barLen
        CHECK (L[boundaryIdx - 1] < 1000.0f,
               "the sample just before the boundary still belongs to the outgoing deck (hard cut, no crossfade)");
        CHECK (L[boundaryIdx] == 10000.0f,
               "the very next sample is the incoming deck's own sample 0 -- an EXACT hard cut, no gain ramp at all");
    }

    // ---- M5-T4: nextPlay's auto-triggered switch becomes a crossfade when the target is also stem mode ----
    {
        const int barLen = 2000;
        Tempo t { 120.0, 4 };
        const double sr = 1000.0;
        const int stemLen = 300;

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].mode = DeckMode::stem;   // nextPlay (default)
        s.decks[1].mode = DeckMode::stem;
        s.decks[0].layers[0].left = std::vector<float> ((size_t) stemLen, 1.0f);
        s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = std::vector<float> (1000, 2.0f);
        s.decks[1].layers[0].loaded = true;

        std::vector<float> L1 (stemLen + 50), R1 (stemLen + 50);
        s.render (L1.data(), R1.data(), stemLen + 50, 1.0f);
        CHECK (s.queuedDeck() == 1, "nextPlay auto-queues deck 1 once the stem finishes");
        CHECK (s.activeDeck() == 0, "deck stays active until the bar boundary");
        CHECK (! s.isDeckAudible (1), "deck 1 is not yet audible -- the crossfade hasn't started (still before the bar boundary)");

        std::vector<float> L2 (barLen), R2 (barLen);
        s.render (L2.data(), R2.data(), barLen, 1.0f);

        CHECK (s.activeDeck() == 1,
               "the auto-triggered nextPlay switch completes as a crossfade and activeDeck() ends up on deck 1");
    }

    // ---- M5-T4: the crossfade's gain ramp and mixing are block-size independent ----
    {
        const int barLen = 2000;
        Tempo t { 120.0, 4 };
        const double sr = 1000.0;

        std::vector<float> deck0Content (3000, 1.0f);
        std::vector<float> deck1Content (3000, 2.0f);

        auto runOnce = [&] (int chunkSize)
        {
            Session<2> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].mode = DeckMode::stem;
            s.decks[1].mode = DeckMode::stem;
            s.decks[0].layers[0].left = deck0Content; s.decks[0].layers[0].loaded = true;
            s.decks[1].layers[0].left = deck1Content; s.decks[1].layers[0].loaded = true;
            s.queueSwitch (1);

            const int total = barLen + 300;
            std::vector<float> L (total), R (total);
            int done = 0;
            while (done < total)
            {
                const int chunk = std::min (chunkSize, total - done);
                s.render (L.data() + done, R.data() + done, chunk, 0.5f);
                done += chunk;
            }
            return L;
        };

        auto bigChunks   = runOnce (10'000);
        auto smallChunks = runOnce (7);   // not a divisor of 120 -- deliberately stresses odd chunk boundaries within the crossfade

        CHECK (bigChunks == smallChunks,
               "the crossfade's gain ramp and mixing are bit-identical regardless of how render() calls are chunked into blocks");
    }

    // ---- M6-T2: Deck::beatsPerBar overrides the switch boundary, bpm untouched ----
    // Session's own tempo says 4 beats/bar (bar = 2000 samples @ this sr);
    // deck 0 is tagged with beatsPerBar=2 (e.g. a 6/8-signature deck per
    // PRD §1's SIGS example), so its OWN bar is half as long (1000 samples).
    // If Deck::beatsPerBar were ignored, the switch would land at 2000, not 1000.
    {
        const double sr = 1000.0;   // 120 BPM -> 1 beat = 0.5s = 500 samples @ this sr
        Tempo t { 120.0, 4 };       // Session's own tempo: 4 beats/bar = 2000 samples

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        s.decks[0].beatsPerBar = 2;   // this deck's OWN signature: 2 beats/bar = 1000 samples
        auto rampA = makeRamp (1000);
        std::vector<float> rampB (1000);
        for (int i = 0; i < 1000; ++i) rampB[(size_t) i] = 10000.0f + (float) i;
        s.decks[0].layers[0].left = rampA; s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = rampB; s.decks[1].layers[0].loaded = true;

        s.queueSwitch (1);   // masterPos 0 is a trivial boundary for EITHER bar length -- render past it first
        std::vector<float> L0 (10), R0 (10);
        s.render (L0.data(), R0.data(), 10, 1.0f);

        // re-queue (the immediate-boundary switch above already fired at
        // masterPos 0) — this time from a genuinely mid-bar position
        CHECK (s.activeDeck() == 1, "sanity: the trivial-boundary switch at masterPos 0 already completed");
        s.decks[1].beatsPerBar = 2;   // give deck 1 the same 1000-sample bar so the NEXT switch is also exactly computable
        s.queueSwitch (0);
        std::vector<float> L (990 + 5), R (990 + 5);
        s.render (L.data(), R.data(), (int) L.size(), 1.0f);

        CHECK (L[989] >= 10000.0f,
               "the sample just before the boundary still belongs to the outgoing deck (deck 1)");
        CHECK (L[990] == rampA[0],
               "the switch lands EXACTLY at deck 1's OWN 1000-sample bar boundary (beatsPerBar=2), not Session's default 2000-sample bar -- "
               "the very next sample is deck 0's own sample 0");
    }

    // ---- M6-T2: Deck::beatsPerBar == 0 (default/unset) falls back to Session's own tempo, unchanged ----
    {
        const int barLen = 1000;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);

        Session<2> s;
        s.setTempo (t);
        s.prepare (sr);
        CHECK (s.decks[0].beatsPerBar == 0, "sanity: a fresh Deck's beatsPerBar defaults to the unset sentinel");

        auto rampA = makeRamp (barLen);
        std::vector<float> rampB (barLen);
        for (int i = 0; i < barLen; ++i) rampB[(size_t) i] = 10000.0f + (float) i;
        s.decks[0].layers[0].left = rampA; s.decks[0].layers[0].loaded = true;
        s.decks[1].layers[0].left = rampB; s.decks[1].layers[0].loaded = true;

        std::vector<float> L0 (10), R0 (10);
        s.render (L0.data(), R0.data(), 10, 1.0f);
        s.queueSwitch (1);
        std::vector<float> L (barLen - 10 + 5), R (barLen - 10 + 5);
        s.render (L.data(), R.data(), (int) L.size(), 1.0f);

        CHECK (L[(size_t) (barLen - 10)] == 10000.0f,
               "with beatsPerBar left unset, the switch still lands at Session's own tempo.beatsPerBar-derived boundary exactly as before Milestone 6");
    }

    // ---- M6-T2: findNextDeckWithContent()'s bounded search stays within the given range ----
    {
        Session<4> s;
        s.setTempo ({ 120.0, 4 });
        s.prepare (44100.0);
        // deck 0 active (default). deck 1 (out of range) has content; deck 3
        // (in range [2,2)... i.e. decks 2-3) also has content.
        s.decks[1].layers[0].left = makeRamp (100); s.decks[1].layers[0].loaded = true;
        s.decks[3].layers[0].left = makeRamp (100); s.decks[3].layers[0].loaded = true;

        CHECK (s.findNextDeckWithContent() == 1,
               "sanity: the default, unbounded search finds deck 1 first, exactly as before Milestone 6");
        CHECK (s.findNextDeckWithContent (2, 2) == 3,
               "a bounded search restricted to decks [2,4) skips deck 1 (outside the range) and finds deck 3");
        CHECK (s.findNextDeckWithContent (2, 1) == -1,
               "a bounded search restricted to just deck 2 finds nothing, even though decks 1 and 3 both have content");
    }

    // ---- M6-T2 (amended): nextPlay's internal auto-advance respects Deck::autoAdvanceRangeStart/Count ----
    // deck 0 (active, stem mode, nextPlay) is tagged as belonging to a
    // 2-deck range [0,2) -- deck 1 (in range) has no content, deck 2 (OUT of
    // range) has content. Without the fix, findNextDeckWithContent()'s
    // internal call inside Session::render() would search unbounded and
    // wrongly auto-advance to deck 2.
    {
        Session<3> s;
        s.setTempo ({ 120.0, 4 });
        s.prepare (44100.0);
        const int stemLen = 100;
        s.decks[0].mode = DeckMode::stem;   // nextPlay (default)
        s.decks[0].layers[0].left = makeRamp (stemLen);
        s.decks[0].layers[0].loaded = true;
        s.decks[0].autoAdvanceRangeStart = 0;
        s.decks[0].autoAdvanceRangeCount = 2;   // deck 0's own "signature": decks 0-1 only
        // deck 1 (in range) has nothing loaded
        s.decks[2].layers[0].left = makeRamp (100); s.decks[2].layers[0].loaded = true;   // OUT of range

        std::vector<float> L (stemLen + 50), R (stemLen + 50);
        s.render (L.data(), R.data(), stemLen + 50, 1.0f);

        CHECK (s.queuedDeck() == -1,
               "nextPlay bounded to decks [0,2) finds nothing -- it must NOT auto-advance to deck 2, which is outside its own signature's range");
    }

    // ---- M6-T2 (amended): with the range set, nextPlay DOES find a loaded deck within its own bounds ----
    {
        Session<3> s;
        s.setTempo ({ 120.0, 4 });
        s.prepare (44100.0);
        const int stemLen = 100;
        s.decks[0].mode = DeckMode::stem;
        s.decks[0].layers[0].left = makeRamp (stemLen);
        s.decks[0].layers[0].loaded = true;
        s.decks[0].autoAdvanceRangeStart = 0;
        s.decks[0].autoAdvanceRangeCount = 2;   // decks 0-1
        s.decks[1].layers[0].left = makeRamp (100); s.decks[1].layers[0].loaded = true;   // IN range
        s.decks[2].layers[0].left = makeRamp (100); s.decks[2].layers[0].loaded = true;   // OUT of range, would be found first unbounded

        std::vector<float> L (stemLen + 50), R (stemLen + 50);
        s.render (L.data(), R.data(), stemLen + 50, 1.0f);

        CHECK (s.queuedDeck() == 1,
               "nextPlay bounded to decks [0,2) correctly finds deck 1 within its own range, not deck 2 which is outside it");
    }

    // ---- M7: renderPerTab() sums to render()'s output across a queued hard-cut switch ----
    {
        const int barLen = 1000;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);

        auto rampA = makeRamp (barLen);
        std::vector<float> rampB (barLen);
        for (int i = 0; i < barLen; ++i) rampB[(size_t) i] = 10000.0f + (float) i;

        const int total = barLen * 2 + 100;

        // reference: render()'s existing, proven single-pair output
        std::vector<float> refL (total), refR (total);
        {
            Session<2> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].layers[0].left = rampA; s.decks[0].layers[0].loaded = true;
            s.decks[1].layers[0].left = rampB; s.decks[1].layers[0].loaded = true;
            std::vector<float> L0 (10), R0 (10);
            s.render (L0.data(), R0.data(), 10, 1.0f);
            s.queueSwitch (1);
            s.render (refL.data() + 10, refR.data() + 10, total - 10, 1.0f);
            std::copy (L0.begin(), L0.end(), refL.begin());
            std::copy (R0.begin(), R0.end(), refR.begin());
        }

        // renderPerTab(): identical setup, summed across every tab
        std::array<std::vector<float>, kNumLayers> tabL, tabR;
        std::array<float*, kNumLayers> tabLPtr, tabRPtr;
        for (int i = 0; i < kNumLayers; ++i) { tabL[(size_t) i].assign ((size_t) total, 0.0f); tabR[(size_t) i].assign ((size_t) total, 0.0f); }
        {
            Session<2> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].layers[0].left = rampA; s.decks[0].layers[0].loaded = true;
            s.decks[1].layers[0].left = rampB; s.decks[1].layers[0].loaded = true;

            for (int i = 0; i < kNumLayers; ++i) { tabLPtr[(size_t) i] = tabL[(size_t) i].data(); tabRPtr[(size_t) i] = tabR[(size_t) i].data(); }
            s.renderPerTab (tabLPtr, tabRPtr, 10);
            s.queueSwitch (1);
            for (int i = 0; i < kNumLayers; ++i) { tabLPtr[(size_t) i] = tabL[(size_t) i].data() + 10; tabRPtr[(size_t) i] = tabR[(size_t) i].data() + 10; }
            s.renderPerTab (tabLPtr, tabRPtr, total - 10);
        }

        bool matches = true;
        for (int i = 0; i < total && matches; ++i)
        {
            float sumL = 0.0f, sumR = 0.0f;
            for (int t2 = 0; t2 < 4; ++t2) { sumL += tabL[(size_t) t2][(size_t) i]; sumR += tabR[(size_t) t2][(size_t) i]; }
            if (std::fabs (sumL - refL[(size_t) i]) > 1e-4f || std::fabs (sumR - refR[(size_t) i]) > 1e-4f) matches = false;
        }
        CHECK (matches, "renderPerTab()'s 4 summed tab streams exactly reproduce render()'s single-pair output across a queued hard-cut switch");
    }

    // ---- M7: renderPerTab() sums to render()'s output across a stem-to-stem crossfade ----
    {
        Tempo t { 120.0, 4 };
        const double sr = 1000.0;
        const int crossfadeLen = 120;
        const int total = crossfadeLen + 50;

        std::vector<float> refL (total), refR (total);
        {
            Session<2> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].mode = DeckMode::stem;
            s.decks[1].mode = DeckMode::stem;
            s.decks[0].layers[0].left = std::vector<float> (5000, 1.0f); s.decks[0].layers[0].loaded = true;
            s.decks[1].layers[0].left = std::vector<float> (5000, 2.0f); s.decks[1].layers[0].loaded = true;
            s.queueSwitch (1);
            s.render (refL.data(), refR.data(), total, 1.0f);
        }

        std::array<std::vector<float>, kNumLayers> tabL, tabR;
        std::array<float*, kNumLayers> tabLPtr, tabRPtr;
        for (int i = 0; i < kNumLayers; ++i)
        {
            tabL[(size_t) i].assign ((size_t) total, 0.0f);
            tabR[(size_t) i].assign ((size_t) total, 0.0f);
            tabLPtr[(size_t) i] = tabL[(size_t) i].data();
            tabRPtr[(size_t) i] = tabR[(size_t) i].data();
        }
        {
            Session<2> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].mode = DeckMode::stem;
            s.decks[1].mode = DeckMode::stem;
            s.decks[0].layers[0].left = std::vector<float> (5000, 1.0f); s.decks[0].layers[0].loaded = true;
            s.decks[1].layers[0].left = std::vector<float> (5000, 2.0f); s.decks[1].layers[0].loaded = true;
            s.queueSwitch (1);
            s.renderPerTab (tabLPtr, tabRPtr, total);
        }

        bool matches = true;
        for (int i = 0; i < total && matches; ++i)
        {
            float sumL = 0.0f, sumR = 0.0f;
            for (int t2 = 0; t2 < 4; ++t2) { sumL += tabL[(size_t) t2][(size_t) i]; sumR += tabR[(size_t) t2][(size_t) i]; }
            if (std::fabs (sumL - refL[(size_t) i]) > 1e-3f || std::fabs (sumR - refR[(size_t) i]) > 1e-3f) matches = false;
        }
        CHECK (matches, "renderPerTab()'s 4 summed tab streams exactly reproduce render()'s single-pair crossfade output");
    }

    // ---- M7: renderPerTab() is block-size independent across a queued switch ----
    {
        const int barLen = 1000;
        Tempo t { 120.0, 4 };
        const double sr = (double) barLen / ((60.0 / t.bpm) * t.beatsPerBar);
        auto rampA = makeRamp (barLen);
        auto rampB = makeRamp (barLen);

        auto runOnce = [&] (int chunkSize)
        {
            Session<2> s;
            s.setTempo (t);
            s.prepare (sr);
            s.decks[0].layers[0].left = rampA; s.decks[0].layers[0].loaded = true;
            s.decks[1].layers[0].left = rampB; s.decks[1].layers[0].loaded = true;

            const int total = barLen + 300;
            std::array<std::vector<float>, kNumLayers> tabL, tabR;
            for (int i = 0; i < kNumLayers; ++i) { tabL[(size_t) i].assign ((size_t) total, 0.0f); tabR[(size_t) i].assign ((size_t) total, 0.0f); }

            bool fired = false;
            int done = 0;
            while (done < total)
            {
                int chunk = std::min (chunkSize, total - done);
                if (! fired) chunk = std::min (chunk, (barLen + 100) - done);

                std::array<float*, kNumLayers> ptrsL, ptrsR;
                for (int i = 0; i < kNumLayers; ++i) { ptrsL[(size_t) i] = tabL[(size_t) i].data() + done; ptrsR[(size_t) i] = tabR[(size_t) i].data() + done; }
                s.renderPerTab (ptrsL, ptrsR, chunk);
                done += chunk;

                if (! fired && done >= barLen + 100) { s.queueSwitch (1); fired = true; }
            }
            return tabL;
        };

        auto bigChunks   = runOnce (10'000);
        auto smallChunks = runOnce (37);

        bool identical = true;
        for (int i = 0; i < 4 && identical; ++i)
            if (bigChunks[(size_t) i] != smallChunks[(size_t) i]) identical = false;
        CHECK (identical, "renderPerTab()'s output is bit-identical regardless of how render() calls are chunked into blocks, including across a queued switch");
    }

    std::printf ("\n%d/%d tests passed\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
