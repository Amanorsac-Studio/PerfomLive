// ============================================================================
//  OneShotVoice.h — a single, independent one-shot/loop player (Pads & FX,
//  PRODUCT_REQUIREMENTS.md §8/§9; ARCHITECTURE.md's resolved Architecture
//  Decision Pending #3).
//
//  Pure C++, no JUCE. Structurally a simpler cousin of a Deck layer (one
//  buffer, a gain, an optional fade-in/out envelope) but deliberately NOT a
//  Deck/Layer -- it owns its OWN playhead (not shared with 3 others), isn't
//  bar-quantized, and needs a click-free STOP mechanism Deck has no
//  equivalent for.
//
//  Click-free stop (PRODUCT_REQUIREMENTS.md §8): captures the TRUE
//  instantaneous envelope value the moment a stop is requested, then rides
//  an exponential curve down to a floor, then a short linear tail to exactly
//  zero. Every sample's envelope is computed directly here, every block --
//  unlike the JS reference's Web Audio AudioParam automation (whose stale
//  cached .value on read was the actual historical defect,
//  EzPlay-Audio-Engine-Report.md §7.2/§10.4), there is no separate
//  automation-curve object to read a stale value from in this engine, so
//  that specific hazard cannot recur here by construction -- the curve SHAPE
//  is still reproduced faithfully, deliberately, not just approximated.
//
//  Real-time safety / thread safety: trigger()/requestStop() are called from
//  the message thread and only ever SET an std::atomic<bool> request flag --
//  mirroring Layer::pendingSwapReady's established pattern (Deck.h). The
//  audio thread (render()) is the ONLY writer of playhead/envelope/state; it
//  observes and consumes each request flag at the start of its own render()
//  call. isActive() is a separate std::atomic<bool>, written only by the
//  audio thread, safely readable from the message thread (e.g. for
//  exclusivity/UI display) without racing render()'s own internal state.
// ============================================================================
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace ezdeck
{

class OneShotVoice
{
public:
    std::vector<float> left, right;   // empty right -> mono, duplicates left (Layer's own convention)
    bool  loaded { false };
    bool  loop   { false };            // PRD §8/§9: pads/FX may be flagged to loop; a non-looping voice stops itself at its own natural end
    float gain          { 1.0f };
    int   fadeInSamples  { 0 };        // 0 = no fade-in (Layer's own convention, reused verbatim)
    int   fadeOutSamples { 0 };        // 0 = no fade-out at the clip's own natural end

    // PerformLive UI/UX Design Notes (Studio One reference): "each pad
    // supports... Volume/Mute/Solo." Set from the message thread, read every
    // sample on the audio thread -- same cross-thread shape as
    // triggerRequested/active below (a plain bool here would risk a torn
    // read audible as a click), so these are atomic even though gain/loop
    // above are plain (pre-existing, undisturbed). Gate only, never a state
    // transition: muting/soloing never stops playhead advance or the
    // click-free stop envelope, it only zeroes the sample this voice
    // contributes -- the same non-destructive relationship Mixer.h's own
    // channel gain/mute/solo already has to its post-fader signal.
    std::atomic<bool> enabled { true };    // false = muted
    std::atomic<bool> soloed  { false };

    // Sizes the click-free stop envelope's timing constants from the real
    // device sample rate -- called once in prepare(), never inside render().
    void prepare (double sampleRate)
    {
        stopExpSamples = std::max (1, (int) std::llround (kStopExpSeconds * sampleRate));
        stopLinSamples = std::max (1, (int) std::llround (kStopLinSeconds * sampleRate));
        stopDecayPerSample = std::pow (kStopFloor, 1.0 / (double) stopExpSamples);
        deviceSampleRate = sampleRate;
        updateRateRatio();
    }

    // Owner bug ("all the inserted pads are playing at a pitch higher than
    // the key the way imported and detuned"): render() used to advance the
    // playhead exactly 1 buffer sample per OUTPUT sample -- correct only
    // when the file's rate happens to equal the device's. A 44.1k file on a
    // 48k device played ~8.8% fast (~1.5 semitones sharp). Same fix Deck
    // has always had: a rateRatio, consumed fractionally by render()'s
    // existing linear interpolation. Message thread only (set at clip load,
    // like trigger()/requestStop() -- a plain double, same precedent as
    // Deck::setRateRatio). 0 = unknown file rate -> play 1:1, the exact
    // pre-fix behaviour, so nothing changes for callers that never set it.
    void setFileSampleRate (double sr)
    {
        fileSampleRate = sr;
        updateRateRatio();
    }

    // Message thread only. Starts (or retriggers) playback from sample 0.
    // Does not touch playhead/state directly -- the audio thread applies
    // this at the start of its next render() call.
    void trigger()
    {
        triggerRequested.store (true, std::memory_order_release);
        stopRequested.store (false, std::memory_order_release);   // a fresh trigger cancels any pending stop
    }

    // Message thread only. Requests a click-free stop -- a no-op if the
    // voice isn't currently playing.
    void requestStop() { stopRequested.store (true, std::memory_order_release); }

    // Safe to call from any thread: true whenever this voice is
    // contributing (or fading out) audio right now.
    bool isActive() const { return active.load (std::memory_order_acquire); }

    // Audio thread only. ACCUMULATES into outL/outR (does not overwrite) --
    // VoiceBank sums many voices into one channel stream, matching this
    // convention throughout. Safe to call with any block size; state
    // carries over exactly between calls.
    // anySoloedInBank: VoiceBank's own per-block "is any voice in this bank
    // currently soloed" snapshot (see VoiceBank::render()) -- same
    // soloActive-then-per-channel-check shape Mixer.h's mixDown() already
    // uses, applied one level down at the voice instead of the channel.
    void render (float* outL, float* outR, int numSamples, bool anySoloedInBank = false)
    {
        if (triggerRequested.exchange (false, std::memory_order_acquire))
        {
            playhead = 0.0;
            envelope = 1.0f;
            state = loaded ? State::playing : State::idle;
            active.store (state != State::idle, std::memory_order_release);
        }
        if (stopRequested.exchange (false, std::memory_order_acquire) && state == State::playing)
        {
            state = State::stopping;
            stopPhase = StopPhase::expDecay;
        }

        if (state == State::idle || ! loaded) return;

        const int len = (int) left.size();
        if (len <= 0) { state = State::idle; active.store (false, std::memory_order_release); return; }

        // Read once per block, not per-sample -- neither flag can change
        // mid-block in a way that matters acoustically, so one relaxed load
        // each is enough (matches Mixer.h's own per-block channel snapshot).
        const bool audible = enabled.load (std::memory_order_relaxed)
                              && (! anySoloedInBank || soloed.load (std::memory_order_relaxed));
        const float audibleGain = audible ? 1.0f : 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            if (state == State::idle) break;

            double pos = playhead;
            if (loop)
            {
                pos = std::fmod (playhead, (double) len);
                if (pos < 0.0) pos += len;
            }
            else if (playhead >= (double) len)
            {
                state = State::idle;
                active.store (false, std::memory_order_release);
                break;
            }

            const int   idx0 = (int) pos;
            const int   idx1 = loop ? (idx0 + 1) % len : std::min (len - 1, idx0 + 1);
            const float frac = (float) (pos - idx0);

            const float l0 = left[(size_t) idx0];
            const float l1 = left[(size_t) idx1];
            const float l  = l0 + frac * (l1 - l0);

            float r;
            if (! right.empty())
            {
                // Defence in depth: `right` is clamped INDEPENDENTLY of
                // `left` (Deck::render's own long-standing convention).
                // len comes from left.size(); if the two ever disagree --
                // a stale channel from a previous load, a partially-applied
                // buffer swap -- indexing `right` by a left-derived position
                // would read past its end, on the audio thread. Clamping
                // here makes that structurally impossible rather than
                // relying on every loader to keep the two in step.
                const int rLast = (int) right.size() - 1;
                const float r0 = right[(size_t) std::min (idx0, rLast)];
                const float r1 = right[(size_t) std::min (idx1, rLast)];
                r = r0 + frac * (r1 - r0);
            }
            else
            {
                r = l;
            }

            // natural-end fade-in/out, at the clip's own boundaries --
            // Layer's own exact convention, reused verbatim.
            float clipEnvelope = 1.0f;
            if (fadeInSamples > 0 && pos < (double) fadeInSamples)
                clipEnvelope = (float) (pos / (double) fadeInSamples);
            if (! loop && fadeOutSamples > 0 && pos > (double) (len - fadeOutSamples))
            {
                const float outEnv = (float) (((double) len - pos) / (double) fadeOutSamples);
                clipEnvelope = std::min (clipEnvelope, outEnv);
            }

            // click-free stop envelope, independent of (multiplicative with)
            // the natural clip fade above -- see this file's header comment.
            if (state == State::stopping)
            {
                if (stopPhase == StopPhase::expDecay)
                {
                    envelope = (float) ((double) envelope * stopDecayPerSample);
                    if (envelope <= kStopFloor) { stopPhase = StopPhase::linTail; stopLinCounter = 0; stopLinStart = envelope; }
                }
                else
                {
                    ++stopLinCounter;
                    envelope = stopLinStart * (1.0f - (float) stopLinCounter / (float) stopLinSamples);
                    if (stopLinCounter >= stopLinSamples)
                    {
                        envelope = 0.0f;
                        state = State::idle;
                        active.store (false, std::memory_order_release);
                    }
                }
            }

            const float g = gain * clipEnvelope * envelope * audibleGain;
            outL[i] += l * g;
            outR[i] += r * g;

            playhead += rateRatio;   // file-rate/device-rate conversion (see setFileSampleRate) -- still NO tempo warp (PRD §8/§9): this is pitch-correct playback, not stretching
        }
    }

private:
    enum class State    { idle, playing, stopping };
    enum class StopPhase { expDecay, linTail };

    void updateRateRatio()
    {
        rateRatio = (fileSampleRate > 0.0 && deviceSampleRate > 0.0)
                      ? fileSampleRate / deviceSampleRate : 1.0;
    }

    double fileSampleRate   { 0.0 };       // 0 = unknown -- play 1:1 (pre-fix behaviour)
    double deviceSampleRate { 44100.0 };
    double rateRatio        { 1.0 };       // buffer samples advanced per output sample

    static constexpr double kStopFloor      = 0.001;    // ~-60dB
    static constexpr double kStopExpSeconds = 0.050;    // exponential decay to the floor
    static constexpr double kStopLinSeconds = 0.005;    // short linear tail, floor -> exactly 0

    std::atomic<bool> triggerRequested { false };
    std::atomic<bool> stopRequested    { false };
    std::atomic<bool> active           { false };

    // Audio-thread-owned only; never touched by trigger()/requestStop().
    State     state    { State::idle };
    StopPhase stopPhase { StopPhase::expDecay };
    double playhead { 0.0 };
    float  envelope { 1.0f };
    float  stopLinStart { 0.0f };
    int    stopLinCounter { 0 };

    int    stopExpSamples { 1 };
    int    stopLinSamples { 1 };
    double stopDecayPerSample { 1.0 };
};

// A fixed-size bank of NumVoices independent OneShotVoices (PRD §8/§9: 12
// pads, 12 FX slots -- structurally identical types, differing only in
// `exclusive`).
template <int NumVoices>
class VoiceBank
{
public:
    std::array<OneShotVoice, NumVoices> voices;

    // Pads default true ("exclusive by default", PRD §8); FX default false
    // (no such constraint anywhere in PRD §9). The "One pad at a time"
    // settings toggle itself is Milestone 13's job -- this is the hardcoded
    // default behavior, not the toggle.
    bool exclusive { false };

    void prepare (double sampleRate) { for (auto& v : voices) v.prepare (sampleRate); }

    // Message thread only. If exclusive, requests a click-free stop on
    // every OTHER currently-active voice before triggering this one --
    // "starting one pad fades the others out" (PRD §8), never an abrupt cut.
    void triggerVoice (int idx)
    {
        if (exclusive)
            for (int i = 0; i < NumVoices; ++i)
                if (i != idx) voices[(size_t) i].requestStop();
        voices[(size_t) idx].trigger();
    }

    void stopVoice (int idx) { voices[(size_t) idx].requestStop(); }

    // Audio thread only. Overwrites (not accumulates) outL/outR with the sum
    // of all NumVoices voices -- matches Deck::render()'s own caller-facing
    // convention. Zero allocation.
    void render (float* outL, float* outR, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
        bool anySoloed = false;
        for (auto& v : voices)
            if (v.soloed.load (std::memory_order_relaxed)) { anySoloed = true; break; }
        for (auto& v : voices) v.render (outL, outR, numSamples, anySoloed);
    }
};

} // namespace ezdeck
