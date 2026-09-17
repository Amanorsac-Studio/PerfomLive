// ============================================================================
//  Session.h — bar-quantized switching between multiple decks.
//
//  Pure C++, no JUCE. A Session owns N decks (see Deck.h) plus a master
//  clock: a monotonic sample counter and a tempo/time-signature pair that
//  together define where bar boundaries fall. Exactly one deck is "active"
//  (audible) at a time. Queuing another deck does not switch immediately —
//  the switch is held until the next bar boundary, so the change always
//  lands on the beat instead of chopping the current deck mid-phrase. When
//  the new deck becomes active it is reset(), so its layers start at
//  playhead 0 — phase-aligned to bar zero, exactly like the 4-layer lock
//  inside a single Deck.
//
//  Kept JUCE-free for the same reason as Deck.h and EzDSP.h: the switching
//  math is exactly the part that must be provably correct in sample numbers,
//  not by ear — see switchtest.cpp.
// ============================================================================
#pragma once
#include "Deck.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ezdeck
{

struct Tempo
{
    double bpm         { 120.0 };
    int    beatsPerBar { 4 };
};

inline int64_t barLengthSamples (const Tempo& tempo, double sampleRate)
{
    const double secondsPerBar = (60.0 / tempo.bpm) * (double) tempo.beatsPerBar;
    return (int64_t) std::llround (secondsPerBar * sampleRate);
}

// The smallest multiple of barLen that is >= pos. If pos already sits exactly
// on a boundary, that same position is returned (an immediate switch is valid).
inline int64_t nextBarBoundaryAtOrAfter (int64_t pos, int64_t barLen)
{
    if (barLen <= 0) return pos;
    const int64_t rem = pos % barLen;
    return rem == 0 ? pos : pos + (barLen - rem);
}

template <int NumDecks>
class Session
{
public:
    std::array<Deck, NumDecks> decks;

    Session() { resizeCrossfadeBuffers(); }

    void setTempo (Tempo t) { tempo = t; }
    Tempo getTempo() const  { return tempo; }

    void prepare (double sampleRate)
    {
        deviceSampleRate = sampleRate;
        resizeCrossfadeBuffers();
    }

    // Milestone 5 (M5-T4): `activeDeck()` names the *logical* active deck --
    // the one a switch/re-warp/UI status line should treat as the
    // performance's current selection. It does not change meaning during a
    // crossfade: it still names the deck being faded OUT, exactly as it
    // would for an ordinary, still-in-flight hard-cut switch today (Model B,
    // ARCHITECTURE.md's resolved Architecture Decision Pending #8).
    int      activeDeck()    const { return active; }
    int      queuedDeck()    const { return queued; }

    // ---- playback control for sectioned stems (AbleSet-style) --------------
    //
    // Three things a performance view needs that a loop-launcher does not:
    // a QUEUED seek that fires on a musical boundary, a STOP that fires when
    // the playhead reaches a point ("+PAUSE"), and a COUNT-IN. All three are
    // armed from the message thread and resolved here, inside the chunked
    // render loop, at the exact sample -- the same place queued deck switches
    // already resolve, using the same chunk-limiting trick, so "at the next
    // bar" means the same thing for a section jump as for a deck switch.
    //
    // Target/fire positions are in the ACTIVE deck's playhead units (layer
    // samples); the caller converts bars to samples. Only meaningful while
    // the active deck is in stem mode; a loop-mode deck ignores all of it.

    enum class SeekWhen { now, nextBar, atDeckPosition };

    /** Arm a jump to target. For atDeckPosition, it fires when the deck's
        playhead reaches fireAt (e.g. the end of the current section). */
    void queueSeek (double target, SeekWhen when, double fireAt = 0.0)
    {
        seekTarget = std::max (0.0, target);
        seekFireAt = std::max (0.0, fireAt);
        seekWhen   = when;
        seekPending.store (true, std::memory_order_release);
    }
    void cancelSeek()                { seekPending.store (false, std::memory_order_release); }
    bool hasPendingSeek() const      { return seekPending.load (std::memory_order_acquire); }
    double pendingSeekTarget() const { return seekTarget; }

    /** Stop playback (signalled to the owner via consumeStopRequest()) when
        the active deck's playhead reaches deckPos. */
    void armStopAt (double deckPos)
    {
        stopAtPos = std::max (0.0, deckPos);
        stopArmed.store (true, std::memory_order_release);
    }
    void cancelStop()           { stopArmed.store (false, std::memory_order_release); }
    bool isStopArmed() const    { return stopArmed.load (std::memory_order_acquire); }

    /** True exactly once after an armed stop fires. The owner stops the
        transport; Session itself has no play/stop concept. */
    bool consumeStopRequest()   { return stopRequested.exchange (false, std::memory_order_acq_rel); }

    /** After an armed stop fires, Session stays silent until this is called
        (or until resetTransport()/switchNow(), which imply a fresh start). */
    void clearHalt()     { halted.store (false, std::memory_order_release); }
    bool isHalted() const { return halted.load (std::memory_order_acquire); }

    /** Play samples of silence from the deck while the master clock runs,
        so the metronome can count the band in. The deck's playhead does not
        move until the count-in ends. */
    void startCountIn (int64_t samples) { countInRemaining = std::max<int64_t> (0, samples); }
    bool isCountingIn() const           { return countInRemaining > 0; }
    int64_t countInSamplesLeft() const  { return countInRemaining; }

    /** The tempo a given deck renders against (master bpm, its own beats-per-bar). */
    Tempo deckTempo (int deckIdx) const { return effectiveTempo (deckIdx); }
    int64_t  masterPosition() const { return masterPos; }

    // Milestone 5 (M5-T4): a narrower, different question than activeDeck()
    // -- "is the audio thread actually reading this deck's layers right
    // now." Outside any crossfade the two always agree; they diverge only
    // for the crossfade's destination deck, and only for its bounded
    // transition window. Stem-to-stem crossfade is the one, sole,
    // intentional exception to the steady-state rule that exactly one deck
    // is audible at a time -- every other code path in this engine
    // (loop-mode switches, re-warp scheduling, preview playback) continues
    // to assume and enforce that rule unmodified.
    bool isDeckAudible (int deckIdx) const
    {
        return deckIdx == active || (crossfade.active && deckIdx == crossfade.toDeck);
    }

    // Request a switch to deckIndex. Takes effect at the next bar boundary
    // measured from the CURRENT master clock position. No-op if that deck is
    // already active.
    void queueSwitch (int deckIndex)
    {
        if (deckIndex != active) queued = deckIndex;
    }

    // SPEC_PERFORM_V2 GROUP A: promotes deckIndex to active IMMEDIATELY,
    // bypassing the bar-boundary wait queueSwitch()'s callers rely on.
    // queueSwitch() quantizes specifically to avoid chopping an in-flight
    // phrase on the deck currently playing -- but render() (the only place
    // that resolves a queued switch) never runs while the transport is
    // stopped, so a queued switch made while nothing is playing would never
    // resolve until playback resumed, and even then only at whatever bar
    // boundary a frozen masterPos happened to imply. When nothing is
    // audible there is no phrase to protect, so applying the switch right
    // away is strictly more correct, not a shortcut -- this mirrors
    // render()'s own hard-cut promotion exactly (reset the target deck,
    // same phase-lock-to-bar-zero guarantee, clear queued/rewarpPending),
    // just without the wait. Not used for the already-playing case --
    // queueSwitch() there is completely unchanged and still quantizes.
    void switchNow (int deckIndex)
    {
        halted.store (false, std::memory_order_release);
        crossfade.active = false;   // no in-flight fade can be meaningful once nothing is playing
        // Owner round 4: "always start row from beginning" -- the reset is
        // unconditional now. Previously a same-deck switchNow was a complete
        // no-op for the playhead, so re-triggering the already-active row
        // after a stop RESUMED mid-file instead of restarting. Every
        // switchNow() caller runs while nothing is audible (that is this
        // function's documented contract above), so there is no phrase to
        // protect and no audio-thread race.
        decks[(size_t) deckIndex].reset();
        active = deckIndex;
        queued = -1;
        rewarpPending = false;
        tempoChangePending.store (false, std::memory_order_release);
    }

    // Owner round 4: "always start row from beginning when I pause and start
    // playing." Full transport rewind -- master clock to 0 (bar 1, beat 1),
    // every deck's playhead to 0, any queued switch / pending re-warp /
    // in-flight crossfade cleared. Message thread only, and only while the
    // transport is stopped (render() is not being called), same invariant
    // switchNow() above already relies on. masterPos is private with no
    // other setter -- restarting at bar 1 is unreachable from the UI without
    // this, which is why it lives here.
    void resetTransport()
    {
        halted.store (false, std::memory_order_release);
        masterPos = 0;
        barOrigin.store (0, std::memory_order_relaxed);
        tempoChangePending.store (false, std::memory_order_release);
        queued = -1;
        rewarpPending = false;
        crossfade.active = false;
        for (auto& d : decks) d.reset();
    }

    // Milestone 5 (M5-T3): the next deck (after `active`, wrapping around,
    // never `active` itself) that "actually has clips" -- PRODUCT_REQUIREMENTS.md
    // §3's own phrase for the auto-advance skip rule -- loaded regardless of
    // whether any of its layers happen to be currently enabled/muted.
    // Returns -1 if no other deck qualifies.
    //
    // Milestone 6 (M6-T2): rangeStart/rangeCount optionally bound the search
    // to a contiguous sub-range (e.g. one signature's own 8 decks), per
    // ARCHITECTURE.md's resolved Architecture Decision Pending #1 -- PRD §1's
    // signature banks must stay independent "without interfering with each
    // other," so auto-advance must not wander into an unrelated bank.
    // rangeCount <= 0 (the default) reproduces the original whole-array
    // search exactly -- every existing caller is unaffected.
    int findNextDeckWithContent (int rangeStart = 0, int rangeCount = -1) const
    {
        const int count = (rangeCount > 0) ? rangeCount : NumDecks;
        for (int offset = 1; offset <= NumDecks; ++offset)
        {
            const int idx = (active + offset) % NumDecks;
            if (idx == active) break;
            if (idx < rangeStart || idx >= rangeStart + count) continue;
            for (auto& layer : decks[(size_t) idx].layers)
                if (layer.loaded) return idx;
        }
        return -1;
    }

    // Milestone 4 (M4-T7): schedules application of the ACTIVE deck's
    // pending layer swaps (staged via Layer::stagePendingSwap, see Deck.h)
    // at the next bar boundary, mirroring queueSwitch's own bar-quantized
    // semantics. Resolves ARCHITECTURE.md's Architecture Decision Pending
    // #2's Session-level half. Not required or meaningful for an inactive
    // deck -- per that same resolved decision, callers apply an inactive
    // deck's pending swaps directly via decks[i].applyPendingSwaps(),
    // since the audio thread never reads an inactive deck's layers at all.
    void scheduleActiveDeckRewarp() { rewarpPending = true; }

    // Owner: "while a loop or stems are playing I press plus and the tempo
    // goes up in the next bar." At the active deck's next bar: its staged
    // layer swaps (re-stretched to newBpm by the caller) go in, the playhead
    // and every position waiting on it move to the same musical place
    // (x positionScale = new buffer length / old), the tempo becomes newBpm,
    // and bars count from that moment. Message thread; the audio thread
    // resolves it, sample-exactly, like a queued switch.
    void scheduleTempoChange (double newBpm, double positionScale)
    {
        pendingTempoBpm.store (newBpm, std::memory_order_relaxed);
        pendingPositionScale.store (positionScale, std::memory_order_relaxed);
        tempoChangePending.store (true, std::memory_order_release);
    }
    void cancelTempoChange()              { tempoChangePending.store (false, std::memory_order_release); }
    bool isTempoChangePending() const     { return tempoChangePending.load (std::memory_order_acquire); }
    /** How many tempo changes have been applied: the caller watches this to follow along. */
    uint32_t tempoChangesApplied() const  { return tempoChangeCount.load (std::memory_order_relaxed); }
    /** Master clock position where bar 1 of the current tempo began (0 until a tempo change). */
    int64_t barOriginPosition() const     { return barOrigin.load (std::memory_order_relaxed); }

    // Renders numSamples of stereo output, splitting the block internally at
    // the bar boundary if a queued switch falls inside it. Safe for any block
    // size: the boundary is computed from the absolute master sample count,
    // so chunking the call differently never shifts where the switch lands.
    void render (float* outL, float* outR, int numSamples, float masterGain = 0.5f)
    {
        if (halted.load (std::memory_order_acquire))
        {
            for (int i = 0; i < numSamples; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
            return;
        }
        int done = 0;
        while (done < numSamples)
        {
            int chunk = numSamples - done;

            // A crossfade in progress owns the switch/rewarp decision space
            // exclusively until it concludes -- neither block below acts
            // while one is active (see the crossfade branch further down,
            // which is the sole thing advancing `done`/`masterPos` then).
            if (queued >= 0 && ! crossfade.active)
            {
                const int64_t barLen      = barLengthSamples (effectiveTempo (active), deviceSampleRate);
                const int64_t boundary    = nextBarFromOrigin (barLen);
                const int64_t untilSwitch = boundary - masterPos;

                if (untilSwitch <= 0)
                {
                    // Milestone 5 (M5-T4): a stem-to-stem handoff starts a
                    // crossfade instead of a hard cut; every other
                    // combination (at least one loop-mode deck involved)
                    // keeps today's proven, unmodified hard cut --
                    // Milestone 2/3's own guarantee is untouched.
                    if (decks[(size_t) active].mode == DeckMode::stem
                        && decks[(size_t) queued].mode == DeckMode::stem)
                    {
                        crossfade.active   = true;
                        crossfade.fromDeck = active;
                        crossfade.toDeck   = queued;
                        crossfade.startPos = masterPos;
                        decks[(size_t) crossfade.toDeck].reset();
                        // `active` deliberately does NOT flip yet -- see
                        // activeDeck()'s own comment and Architecture
                        // Decision Pending #8 (Model B).
                    }
                    else
                    {
                        active = queued;
                        decks[(size_t) active].reset();
                    }

                    queued = -1;

                    // A re-warp scheduled against the deck we're switching
                    // AWAY from must not be allowed to misfire against the
                    // newly-active deck on a later render() call -- see
                    // ARCHITECTURE.md's resolved Architecture Decision
                    // Pending #2 for why this must be explicit here. Applies
                    // equally whether this was a hard cut or the start of a
                    // crossfade: either way, `fromDeck`'s own pending
                    // re-warp is no longer safe to resolve as simply as
                    // before (see the `!crossfade.active` guard below).
                    rewarpPending = false;
                }
                else if (untilSwitch < (int64_t) chunk)
                {
                    chunk = (int) untilSwitch;
                }
            }

            // Deliberately skipped for the crossfade's own duration -- see
            // this block's own comment where rewarpPending is cleared
            // above. Resolving a re-warp for `fromDeck` mid-fade would
            // reset its playhead while its audio is still audibly fading
            // out, an audible glitch; deferring it is a documented,
            // deliberate simplification, not an oversight.
            if (rewarpPending && ! crossfade.active)
            {
                const int64_t barLen      = barLengthSamples (effectiveTempo (active), deviceSampleRate);
                const int64_t boundary    = nextBarFromOrigin (barLen);
                const int64_t untilRewarp = boundary - masterPos;

                if (untilRewarp <= 0)
                {
                    decks[(size_t) active].applyPendingSwaps();
                    decks[(size_t) active].reset();
                    rewarpPending = false;
                }
                else if (untilRewarp < (int64_t) chunk)
                {
                    chunk = (int) untilRewarp;
                }
            }

            resolveTempoChange (chunk);   // a +/- tempo change waiting for this bar

            // count-in: the clock runs, the deck does not (see startCountIn)
            if (countInRemaining > 0)
            {
                const int n = (int) std::min<int64_t> ((int64_t) chunk, countInRemaining);
                for (int i = 0; i < n; ++i) { outL[done + i] = 0.0f; outR[done + i] = 0.0f; }
                countInRemaining -= n;
                masterPos += n;
                done += n;
                continue;
            }

            if (! crossfade.active && resolveSectionControl (chunk))
            {
                // an armed stop fired at this exact sample: silence, clock frozen
                for (int i = done; i < numSamples; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
                return;
            }

            if (crossfade.active)
            {
                // Milestone 5 (M5-T4): mix fromDeck (fading 1->0) and
                // toDeck (fading 0->1) into the same output block. Clamped
                // to the fade's own remaining duration first, so this
                // iteration never renders more samples than
                // crossfade.durationSamples -- the exact bound
                // fadeScratchL/R were sized to in resizeCrossfadeBuffers(),
                // guaranteeing no allocation here regardless of how large a
                // block the caller requests.
                const int64_t elapsed   = masterPos - crossfade.startPos;
                int64_t       remaining = crossfade.durationSamples - elapsed;
                if (remaining < 1) remaining = 1;
                if (remaining < (int64_t) chunk) chunk = (int) remaining;

                decks[(size_t) crossfade.fromDeck].render (fadeScratchL.data(), fadeScratchR.data(), chunk, masterGain);
                decks[(size_t) crossfade.toDeck].render   (outL + done, outR + done, chunk, masterGain);

                // Linear ramp, `i / durationSamples` -- the same convention
                // Deck::render()'s own fadeInSamples/fadeOutSamples already
                // use (Deck.h), reused here rather than inventing a
                // different curve shape for this one new case.
                for (int i = 0; i < chunk; ++i)
                {
                    const double toGain   = (double) (elapsed + i) / (double) crossfade.durationSamples;
                    const double fromGain = 1.0 - toGain;
                    outL[done + i] = (float) ((double) outL[done + i] * toGain + (double) fadeScratchL[(size_t) i] * fromGain);
                    outR[done + i] = (float) ((double) outR[done + i] * toGain + (double) fadeScratchR[(size_t) i] * fromGain);
                }

                masterPos += chunk;
                done += chunk;

                if (elapsed + chunk >= crossfade.durationSamples)
                {
                    active = crossfade.toDeck;
                    crossfade.active = false;
                }
            }
            else
            {
                // Milestone 5 (M5-T3): clamp this iteration's chunk so a
                // stem-mode active deck's natural end is always detected at
                // the EXACT sample it occurs, regardless of how large a
                // block the caller requests into render() -- the same
                // block-size-independence discipline `queued`/`rewarpPending`
                // already follow above, applied here to the deck's own stem
                // length. Without this, a large enough numSamples could run
                // the deck hundreds or thousands of samples past its
                // natural end before the check below ever runs, silently
                // delaying (e.g.) the `loop` end-behavior's reset by an
                // audible gap.
                if (decks[(size_t) active].mode == DeckMode::stem && ! decks[(size_t) active].stemFinished())
                {
                    const double ratio     = decks[(size_t) active].getRateRatio();
                    const double remaining = (double) decks[(size_t) active].stemLength()
                                            - decks[(size_t) active].playheadPosition();
                    if (ratio > 0.0 && remaining > 0.0)
                    {
                        const int64_t samplesUntilEnd = (int64_t) std::ceil (remaining / ratio);
                        if (samplesUntilEnd < (int64_t) chunk)
                            chunk = (int) std::max<int64_t> (1, samplesUntilEnd);
                    }
                }

                decks[(size_t) active].render (outL + done, outR + done, chunk, masterGain);
                masterPos += chunk;
                done += chunk;

                // Stem end-behavior triggering. Checked every chunk; each
                // of the three behaviors is naturally idempotent, so
                // re-checking on every iteration while stemFinished() stays
                // true (which it will, until something resets/switches
                // away from this deck) is harmless:
                //  - next:     no action at all -- Deck::render() already
                //              outputs silence once every layer has
                //              individually finished (Deck.h, M5-T2).
                //  - loop:     reset()s the deck's own playhead, which makes
                //              stemFinished() false again immediately.
                //  - nextPlay: queues a switch only if nothing else is
                //              already queued, so it never clobbers an
                //              explicit, user-requested switch, and
                //              re-evaluating it every chunk before the
                //              queued switch actually takes effect is a
                //              no-op (queueSwitch to the same target is
                //              idempotent). The switch it queues may itself
                //              become a crossfade -- see the queued branch
                //              above -- if the auto-selected next deck is
                //              also in stem mode.
                // Always false for a loop-mode deck (Deck::stemFinished()'s
                // own guard), so this is a complete no-op for every deck
                // that isn't explicitly in stem mode -- zero behavior
                // change otherwise.
                if (decks[(size_t) active].stemFinished())
                {
                    switch (decks[(size_t) active].stemEndBehavior)
                    {
                        case StemEndBehavior::next:
                            break;

                        case StemEndBehavior::loop:
                            decks[(size_t) active].reset();
                            break;

                        case StemEndBehavior::nextPlay:
                            if (queued < 0)
                            {
                                // Milestone 6: bounded to this deck's own
                                // signature's range when it's been set
                                // (Deck::autoAdvanceRangeStart/Count), so
                                // auto-advance never crosses into an
                                // unrelated signature bank. Sentinel -1
                                // reproduces the original whole-array search.
                                const auto& d = decks[(size_t) active];
                                const int next = (d.autoAdvanceRangeCount > 0)
                                    ? findNextDeckWithContent (d.autoAdvanceRangeStart, d.autoAdvanceRangeCount)
                                    : findNextDeckWithContent();
                                if (next >= 0) queueSwitch (next);
                            }
                            break;
                    }
                }
            }
        }
    }

    // Milestone 7 (M7): identical scheduling to render() above -- the same
    // queued-switch/rewarp/crossfade/stem-end boundary logic, byte-for-byte
    // the same conditions -- but routes each chunk's audio to 4 separate
    // per-tab stereo streams (via Deck::renderPerTab()) instead of one
    // pre-summed pair, so a Mixer above this Session can route each tab/layer
    // through its own independent channel (ARCHITECTURE.md's resolved
    // Architecture Decision Pending #4). Deliberately a separate, additive
    // method rather than a refactor of render() itself, so render()'s own
    // proven, bit-exact behavior (switchtest.cpp, banktest.cpp) is completely
    // unaffected by this method's existence. No masterGain parameter -- all
    // final gain staging is the Mixer's job now. Every outL[tab]/outR[tab]
    // buffer must already be allocated to numSamples by the caller.
    void renderPerTab (std::array<float*, kNumLayers> outL, std::array<float*, kNumLayers> outR, int numSamples)
    {
        if (halted.load (std::memory_order_acquire))
        {
            for (int t = 0; t < kNumLayers; ++t)
                for (int i = 0; i < numSamples; ++i) { outL[(size_t) t][i] = 0.0f; outR[(size_t) t][i] = 0.0f; }
            return;
        }
        int done = 0;
        while (done < numSamples)
        {
            int chunk = numSamples - done;

            if (queued >= 0 && ! crossfade.active)
            {
                const int64_t barLen      = barLengthSamples (effectiveTempo (active), deviceSampleRate);
                const int64_t boundary    = nextBarFromOrigin (barLen);
                const int64_t untilSwitch = boundary - masterPos;

                if (untilSwitch <= 0)
                {
                    if (decks[(size_t) active].mode == DeckMode::stem
                        && decks[(size_t) queued].mode == DeckMode::stem)
                    {
                        crossfade.active   = true;
                        crossfade.fromDeck = active;
                        crossfade.toDeck   = queued;
                        crossfade.startPos = masterPos;
                        decks[(size_t) crossfade.toDeck].reset();
                    }
                    else
                    {
                        active = queued;
                        decks[(size_t) active].reset();
                    }

                    queued = -1;
                    rewarpPending = false;
                }
                else if (untilSwitch < (int64_t) chunk)
                {
                    chunk = (int) untilSwitch;
                }
            }

            if (rewarpPending && ! crossfade.active)
            {
                const int64_t barLen      = barLengthSamples (effectiveTempo (active), deviceSampleRate);
                const int64_t boundary    = nextBarFromOrigin (barLen);
                const int64_t untilRewarp = boundary - masterPos;

                if (untilRewarp <= 0)
                {
                    decks[(size_t) active].applyPendingSwaps();
                    decks[(size_t) active].reset();
                    rewarpPending = false;
                }
                else if (untilRewarp < (int64_t) chunk)
                {
                    chunk = (int) untilRewarp;
                }
            }

            resolveTempoChange (chunk);   // a +/- tempo change waiting for this bar

            // count-in: the clock runs, the deck does not (see startCountIn)
            if (countInRemaining > 0)
            {
                const int n = (int) std::min<int64_t> ((int64_t) chunk, countInRemaining);
                for (int t = 0; t < kNumLayers; ++t)
                    for (int i = 0; i < n; ++i) { outL[(size_t) t][done + i] = 0.0f; outR[(size_t) t][done + i] = 0.0f; }
                countInRemaining -= n;
                masterPos += n;
                done += n;
                continue;
            }

            if (! crossfade.active && resolveSectionControl (chunk))
            {
                // an armed stop fired at this exact sample: silence, clock frozen
                for (int t = 0; t < kNumLayers; ++t)
                    for (int i = done; i < numSamples; ++i) { outL[(size_t) t][i] = 0.0f; outR[(size_t) t][i] = 0.0f; }
                return;
            }

            if (crossfade.active)
            {
                const int64_t elapsed   = masterPos - crossfade.startPos;
                int64_t       remaining = crossfade.durationSamples - elapsed;
                if (remaining < 1) remaining = 1;
                if (remaining < (int64_t) chunk) chunk = (int) remaining;

                std::array<float*, kNumLayers> fromPtrsL {}, fromPtrsR {};
                std::array<float*, kNumLayers> toPtrsL {}, toPtrsR {};
                for (int t = 0; t < kNumLayers; ++t)
                {
                    fromPtrsL[(size_t) t] = fadeScratchPerTabL[(size_t) t].data();
                    fromPtrsR[(size_t) t] = fadeScratchPerTabR[(size_t) t].data();
                    toPtrsL[(size_t) t]   = outL[(size_t) t] + done;
                    toPtrsR[(size_t) t]   = outR[(size_t) t] + done;
                }

                decks[(size_t) crossfade.fromDeck].renderPerTab (fromPtrsL, fromPtrsR, chunk);
                decks[(size_t) crossfade.toDeck].renderPerTab   (toPtrsL, toPtrsR, chunk);

                // Same linear ramp as render()'s own crossfade branch,
                // applied independently per tab -- the ramp weight is
                // identical across tabs (it's "how much of fromDeck vs
                // toDeck", not a per-tab quantity), only the signal it's
                // applied to differs.
                for (int t = 0; t < kNumLayers; ++t)
                {
                    for (int i = 0; i < chunk; ++i)
                    {
                        const double toGain   = (double) (elapsed + i) / (double) crossfade.durationSamples;
                        const double fromGain = 1.0 - toGain;
                        outL[(size_t) t][done + i] = (float) ((double) outL[(size_t) t][done + i] * toGain + (double) fadeScratchPerTabL[(size_t) t][(size_t) i] * fromGain);
                        outR[(size_t) t][done + i] = (float) ((double) outR[(size_t) t][done + i] * toGain + (double) fadeScratchPerTabR[(size_t) t][(size_t) i] * fromGain);
                    }
                }

                masterPos += chunk;
                done += chunk;

                if (elapsed + chunk >= crossfade.durationSamples)
                {
                    active = crossfade.toDeck;
                    crossfade.active = false;
                }
            }
            else
            {
                if (decks[(size_t) active].mode == DeckMode::stem && ! decks[(size_t) active].stemFinished())
                {
                    const double ratio     = decks[(size_t) active].getRateRatio();
                    const double remaining = (double) decks[(size_t) active].stemLength()
                                            - decks[(size_t) active].playheadPosition();
                    if (ratio > 0.0 && remaining > 0.0)
                    {
                        const int64_t samplesUntilEnd = (int64_t) std::ceil (remaining / ratio);
                        if (samplesUntilEnd < (int64_t) chunk)
                            chunk = (int) std::max<int64_t> (1, samplesUntilEnd);
                    }
                }

                std::array<float*, kNumLayers> ptrsL {}, ptrsR {};
                for (int t = 0; t < kNumLayers; ++t)
                {
                    ptrsL[(size_t) t] = outL[(size_t) t] + done;
                    ptrsR[(size_t) t] = outR[(size_t) t] + done;
                }
                decks[(size_t) active].renderPerTab (ptrsL, ptrsR, chunk);
                masterPos += chunk;
                done += chunk;

                if (decks[(size_t) active].stemFinished())
                {
                    switch (decks[(size_t) active].stemEndBehavior)
                    {
                        case StemEndBehavior::next:
                            break;

                        case StemEndBehavior::loop:
                            decks[(size_t) active].reset();
                            break;

                        case StemEndBehavior::nextPlay:
                            if (queued < 0)
                            {
                                const auto& d = decks[(size_t) active];
                                const int next = (d.autoAdvanceRangeCount > 0)
                                    ? findNextDeckWithContent (d.autoAdvanceRangeStart, d.autoAdvanceRangeCount)
                                    : findNextDeckWithContent();
                                if (next >= 0) queueSwitch (next);
                            }
                            break;
                    }
                }
            }
        }
    }

private:
    // Milestone 5 (M5-T4): tracks an in-progress stem-to-stem crossfade.
    // `fromDeck` is always `active` for the crossfade's whole duration
    // (Model B) -- this struct exists to name `toDeck` and the fade's own
    // timing, not to duplicate what `active` already means.
    struct CrossfadeState
    {
        bool    active          { false };
        int     fromDeck        { -1 };
        int     toDeck          { -1 };
        int64_t startPos        { 0 };      // masterPos when the fade began (== the bar boundary)
        int64_t durationSamples { 5292 };   // resized by resizeCrossfadeBuffers() to match kCrossfadeSeconds @ deviceSampleRate
    };

    static constexpr double kCrossfadeSeconds = 0.120;   // PRODUCT_REQUIREMENTS.md §3's own stated duration

    // Milestone 6 (M6-T2): the bar-length-defining Tempo for a specific
    // deck -- substitutes that deck's own Deck::beatsPerBar (its signature's
    // structural beat count) for this Session's tempo.beatsPerBar when it's
    // been set, per ARCHITECTURE.md's resolved Architecture Decision Pending
    // #1. bpm is never overridden here -- it stays exactly the single,
    // Session-wide master value Milestone 4 established. Never queries
    // anything beyond the Deck this Session already owns -- no dependency on
    // SignatureManager or any layer above this one.
    Tempo effectiveTempo (int deckIdx) const
    {
        Tempo t = tempo;
        if (decks[(size_t) deckIdx].beatsPerBar > 0) t.beatsPerBar = decks[(size_t) deckIdx].beatsPerBar;
        return t;
    }

    // Sized once whenever deviceSampleRate is (re)established -- at
    // construction (using the default 44100.0) and again in prepare() --
    // never inside render(), so the audio thread never allocates
    // (ENGINEERING_PRINCIPLES.md §5; KNOWN_BUGS.md #6's own already-tracked
    // caution against exactly this class of mistake). render()'s own
    // stem-end-style chunk clamp guarantees a crossfade's per-iteration
    // chunk never exceeds crossfade.durationSamples, so these buffers are
    // always large enough once sized here.
    void resizeCrossfadeBuffers()
    {
        crossfade.durationSamples = std::max<int64_t> (1, (int64_t) std::llround (kCrossfadeSeconds * deviceSampleRate));
        fadeScratchL.assign ((size_t) crossfade.durationSamples, 0.0f);
        fadeScratchR.assign ((size_t) crossfade.durationSamples, 0.0f);

        // Milestone 7 (M7): renderPerTab()'s own crossfade scratch -- 4 extra
        // pairs (one per tab), sized identically to fadeScratchL/R above and
        // for the same real-time-safety reason (never resized inside
        // render()/renderPerTab()). fadeScratchL/R themselves are untouched --
        // render() keeps using exactly those, unaffected by this addition.
        for (int t = 0; t < kNumLayers; ++t)
        {
            fadeScratchPerTabL[(size_t) t].assign ((size_t) crossfade.durationSamples, 0.0f);
            fadeScratchPerTabR[(size_t) t].assign ((size_t) crossfade.durationSamples, 0.0f);
        }
    }

    Tempo   tempo;
    double  deviceSampleRate { 44100.0 };
    int64_t masterPos { 0 };
    int     active    { 0 };
    int     queued    { -1 };

    // ---- section-control state (see queueSeek/armStopAt/startCountIn) ------
    std::atomic<bool> seekPending   { false };
    double            seekTarget    { 0.0 };
    double            seekFireAt    { 0.0 };
    SeekWhen          seekWhen      { SeekWhen::now };
    std::atomic<bool> stopArmed     { false };
    double            stopAtPos     { 0.0 };
    std::atomic<bool> stopRequested { false };
    // Set the instant an armed stop fires; every render call after that emits
    // silence and leaves the clock alone until the owner clears it. Without
    // this, audio would keep running for however long the UI takes to notice
    // the stop request -- up to a whole UI tick -- which is exactly the
    // sloppy "stopped a beat late" a +PAUSE must never have.
    std::atomic<bool> halted        { false };
    int64_t           countInRemaining { 0 };

    // Shared by render() and renderPerTab(): applies any armed seek/stop to
    // the active deck at the right sample, shrinking chunk so the boundary
    // lands exactly on a chunk edge. Returns true when the remainder of the
    // block must be SILENT and the clock frozen (a stop just fired).
    bool resolveSectionControl (int& chunk)
    {
        auto& deck = decks[(size_t) active];
        if (deck.mode != DeckMode::stem) return false;

        const double ratio = deck.getRateRatio() > 0.0 ? deck.getRateRatio() : 1.0;

        if (seekPending.load (std::memory_order_acquire))
        {
            int64_t until = 0;
            if (seekWhen == SeekWhen::nextBar)
            {
                const int64_t barLen = barLengthSamples (effectiveTempo (active), deviceSampleRate);
                until = nextBarFromOrigin (barLen) - masterPos;
            }
            else if (seekWhen == SeekWhen::atDeckPosition)
            {
                const double remaining = seekFireAt - deck.playheadPosition();
                until = remaining > 0.0 ? (int64_t) std::ceil (remaining / ratio) : 0;
            }

            if (until <= 0)
            {
                // Clamp to the stem so a stale section past a re-trimmed end
                // cannot park the playhead beyond the audio.
                const double len = (double) deck.stemLength();
                deck.seekTo (len > 0.0 ? std::min (seekTarget, len) : seekTarget);
                seekPending.store (false, std::memory_order_release);
            }
            else if (until < (int64_t) chunk)
            {
                chunk = (int) until;
            }
        }

        if (stopArmed.load (std::memory_order_acquire))
        {
            const double remaining = stopAtPos - deck.playheadPosition();
            const int64_t until = remaining > 0.0 ? (int64_t) std::ceil (remaining / ratio) : 0;
            if (until <= 0)
            {
                stopArmed.store (false, std::memory_order_release);
                stopRequested.store (true, std::memory_order_release);
                halted.store (true, std::memory_order_release);
                return true;
            }
            if (until < (int64_t) chunk) chunk = (int) until;
        }
        return false;
    }
    bool    rewarpPending { false };

    // ---- live tempo change (scheduleTempoChange) --------------------------
    std::atomic<bool>     tempoChangePending   { false };
    std::atomic<double>   pendingTempoBpm      { 120.0 };
    std::atomic<double>   pendingPositionScale { 1.0 };
    std::atomic<uint32_t> tempoChangeCount     { 0 };
    std::atomic<int64_t>  barOrigin            { 0 };   // bars are counted from here

    // The next bar line at or after masterPos, counted from where the current
    // tempo began (so a tempo change mid-song keeps bars on the beat).
    int64_t nextBarFromOrigin (int64_t barLen) const
    {
        const int64_t origin = barOrigin.load (std::memory_order_relaxed);
        return origin + nextBarBoundaryAtOrAfter (masterPos - origin, barLen);
    }

    void applyTempoChange()
    {
        auto& deck = decks[(size_t) active];
        deck.applyPendingSwaps();
        const double scale = pendingPositionScale.load (std::memory_order_relaxed);
        if (scale > 0.0)
        {
            deck.scalePositions (scale);
            seekTarget *= scale;
            seekFireAt *= scale;
            stopAtPos  *= scale;
        }
        tempo.bpm = pendingTempoBpm.load (std::memory_order_relaxed);
        barOrigin.store (masterPos, std::memory_order_relaxed);
        rewarpPending = false;
        tempoChangePending.store (false, std::memory_order_release);
        tempoChangeCount.fetch_add (1, std::memory_order_relaxed);
    }

    // Applies a waiting tempo change at this chunk's start if a bar line is
    // here, or shortens the chunk so the next one starts on it.
    void resolveTempoChange (int& chunk)
    {
        if (! tempoChangePending.load (std::memory_order_acquire) || crossfade.active) return;
        const int64_t barLen = barLengthSamples (effectiveTempo (active), deviceSampleRate);
        const int64_t until  = nextBarFromOrigin (barLen) - masterPos;
        if (until <= 0) applyTempoChange();
        else if (until < (int64_t) chunk) chunk = (int) until;
    }

    CrossfadeState      crossfade;
    std::vector<float>  fadeScratchL;
    std::vector<float>  fadeScratchR;

    // Milestone 7 (M7): renderPerTab()'s own crossfade scratch, one pair per
    // tab -- see resizeCrossfadeBuffers()'s own comment.
    std::array<std::vector<float>, kNumLayers> fadeScratchPerTabL;
    std::array<std::vector<float>, kNumLayers> fadeScratchPerTabR;
};

} // namespace ezdeck
