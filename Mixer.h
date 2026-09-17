// ============================================================================
//  Mixer.h — the two-stage routing/mixdown layer above Session/VoiceBank/
//  Metronome (PRODUCT_REQUIREMENTS.md §10, ARCHITECTURE.md's resolved
//  Architecture Decision Pending #4).
//
//  Pure C++, no JUCE. Owns only channel STATE (gain/mute/solo, atomics) --
//  never audio buffers, never Session/VoiceBank/Metronome themselves. Takes
//  already-rendered per-channel stereo streams and mixes them down to one
//  master pair: source -> channel (gain x mute x solo) -> summed into master
//  bus -> master gain -> device. No sub-groups, no insert chains, no sends --
//  confirmed absent from the entire source material (see
//  project/MILESTONE_7-10_ARCHITECTURE.md's Research findings).
//
//  Milestone 7 built gain/mute/solo mixing math. Milestone 10 (this file's
//  latest revision) adds metering -- PRODUCT_REQUIREMENTS.md §10's
//  `level = min(1, sqrt(rms)*1.35 + peak*0.5)`, peak-hold decay 0.86/frame,
//  post-fader (computed from each channel's already gain/mute/solo-resolved
//  contribution, so a muted or solo'd-out channel reads 0). "Frame" here
//  means a UI refresh tick, not an audio block -- audio blocks vary wildly
//  in size/rate depending on the host, while peak-hold decay implies a
//  stable, fixed-rate visual metronome of its own. mixDown() (audio thread)
//  only ever collects each channel's raw instantaneous rms/peak for the
//  block just rendered; updateAndGetMeterLevel() (message thread, called at
//  the UI's own refresh rate) is what actually decays the peak hold and
//  computes the final display level -- kept in this file, not split into
//  Main.cpp, so the full metering formula stays in one tested place
//  (mixertest.cpp).
//
//  This design also confirms PRODUCT_REQUIREMENTS.md §10's own documented
//  historical defect (an indiscriminate `node.disconnect()` silently
//  killing a permanently-attached meter tap) cannot recur here: there is no
//  "routing graph" to tear down at all -- metering is computed directly
//  inside mixDown() from data already flowing through it, not from a
//  separately-wired analyser node a routing change could ever disconnect.
//
//  Real-time safety: mixDown() performs zero allocation. Every cross-thread
//  control (gain/mute/solo, set from the message thread, read every audio
//  block) is std::atomic from the start -- ENGINEERING_PRINCIPLES.md's
//  cross-thread-state discipline applied to new state from day one, not
//  deferred as a known gap the way KNOWN_BUGS.md #13/#15 were.
// ============================================================================
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace ezdeck
{

// PRODUCT_REQUIREMENTS.md §10: "7 channels: Tabs 1-4 ... Pads, FX, Metro,
// plus a Master strip" -- Master is deliberately not one of these 7 (it's
// the post-sum stage, tracked separately below).
// PX-B: Tab5..Tab8 added alongside the original four. The order still
// matches the UI strip order exactly, which is what the meter refresh loop
// relies on -- the new tabs are inserted BEFORE Pads so that "tab index N
// is channel N" stays true for every layer.
// Guide.h: Cues is the spoken guide voice, beside Metro (the click). Both
// are exempt from solo, like any cue send on a desk.
// Live1..Live4: the live tracks (a mic / DI input, or a hosted instrument).
// They are their own channels, after the eight decks, so a deck column is
// only ever stems -- a live source never replaces a column's audio.
enum class MixerChannel { Tab1, Tab2, Tab3, Tab4, Tab5, Tab6, Tab7, Tab8,
                          Live1, Live2, Live3, Live4,
                          Pads, Fx, Metro, Cues, kCount };
constexpr int kNumMixerChannels = (int) MixerChannel::kCount;
constexpr int kNumLiveTracks    = 4;

// Where a channel plays, as one number (stored in projects as outputRoute).
// Owner: "the output should reflect the outputs on the sound card, and we
// should be able to choose mono or stereo":
//   0                 Main: hardware Out 1/2, through Master
//   1 .. 31           stereo pair N: hardware outputs 2N+1 and 2N+2, direct
//   100 .. 163        mono: hardware output (code - 99) alone, direct; the
//                     channel's left and right are summed into it
// Any number of channels may share an output: they add up.
constexpr int kMaxStereoRoutes   = 32;    // Main + 31 pairs = 64 outputs
constexpr int kMonoRouteBase     = 100;
constexpr int kMaxOutputChannels = 64;

inline bool isValidOutputRoute (int code)
{
    return (code >= 0 && code < kMaxStereoRoutes)
        || (code >= kMonoRouteBase && code < kMonoRouteBase + kMaxOutputChannels);
}

inline bool isMonoOutputRoute (int code) { return code >= kMonoRouteBase && code < kMonoRouteBase + kMaxOutputChannels; }

/** The device output pair a route writes to, and which side of it: -1 both
    (stereo), 0 the pair's first output, 1 its second. An invalid code is Main. */
struct OutputTarget { int pair { 0 }; int side { -1 }; };

inline OutputTarget outputTargetFor (int code)
{
    if (isMonoOutputRoute (code))
    {
        const int channel = code - kMonoRouteBase;
        return { channel / 2, channel % 2 };
    }
    if (code >= 0 && code < kMaxStereoRoutes) return { code, -1 };
    return {};
}

class Mixer
{
public:
    Mixer()
    {
        // Milestone 10 will actually route a Metro signal into this channel;
        // the exemption flag is set here, now, so the channel's identity and
        // its solo-exemption are established together rather than patched in
        // later (ARCHITECTURE.md's resolved Architecture Decision Pending #4).
        channels[(size_t) MixerChannel::Metro].exemptFromSolo = true;
        channels[(size_t) MixerChannel::Cues].exemptFromSolo  = true;
    }

    void  setChannelGain (MixerChannel ch, float gain) { channels[idx (ch)].gain.store (gain, std::memory_order_relaxed); }
    float getChannelGain (MixerChannel ch) const       { return channels[idx (ch)].gain.load (std::memory_order_relaxed); }

    void setChannelMute (MixerChannel ch, bool m) { channels[idx (ch)].mute.store (m, std::memory_order_relaxed); }
    bool getChannelMute (MixerChannel ch) const   { return channels[idx (ch)].mute.load (std::memory_order_relaxed); }

    void setChannelSolo (MixerChannel ch, bool s) { channels[idx (ch)].solo.store (s, std::memory_order_relaxed); }
    bool getChannelSolo (MixerChannel ch) const   { return channels[idx (ch)].solo.load (std::memory_order_relaxed); }

    bool isExemptFromSolo (MixerChannel ch) const { return channels[idx (ch)].exemptFromSolo; }

    void  setMasterGain (float g) { masterGain.store (g, std::memory_order_relaxed); }
    float getMasterGain() const   { return masterGain.load (std::memory_order_relaxed); }

    // SPEC_OUTPUT_ROUTING.md: which output PAIR (0 = Main/Out 1-2, 1 = Out
    // 3-4, ...) this channel's post-gain/mute/solo signal sums into.
    // Real-time-safe (atomic, message-thread write / audio-thread read,
    // same pattern as gain/mute/solo above). The CALLER (Main.cpp's
    // getNextAudioBlock) is responsible for clamping this against however
    // many pairs the device actually has right now -- mixDown() below does
    // the same clamp again, defensively, so a stored index pointing past a
    // since-reduced device's pair count can never write out of bounds.
    void setChannelOutputPair (MixerChannel ch, int pair)
    {
        channels[idx (ch)].outputSide.store (-1, std::memory_order_relaxed);
        channels[idx (ch)].outputPair.store (pair, std::memory_order_relaxed);
    }
    int  getChannelOutputPair (MixerChannel ch) const     { return channels[idx (ch)].outputPair.load (std::memory_order_relaxed); }

    /** A route code (see outputTargetFor): Main, a stereo pair, or one mono output. */
    void setChannelOutputRoute (MixerChannel ch, int code)
    {
        const auto target = outputTargetFor (code);
        channels[idx (ch)].outputSide.store (target.side, std::memory_order_relaxed);
        channels[idx (ch)].outputPair.store (target.pair, std::memory_order_relaxed);
    }
    int getChannelOutputSide (MixerChannel ch) const { return channels[idx (ch)].outputSide.load (std::memory_order_relaxed); }

    // Mixes kNumMixerChannels already-rendered stereo streams down to
    // however many output PAIRS the caller provides (outLPerPair.size() --
    // 1 on an ordinary stereo device, matching this method's exact
    // pre-routing behavior; more on a multi-output interface). Every
    // inL[ch]/inR[ch] and every outLPerPair[p]/outRPerPair[p] buffer must
    // already be allocated to numSamples by the caller -- outLPerPair/
    // outRPerPair are read (their SIZE and pointers), never resized or
    // allocated into here, preserving this function's existing zero-
    // allocation real-time guarantee. Overwrites (zeroes then sums into),
    // does not accumulate onto whatever was already in, each pair's buffer.
    //
    // Signal flow per sample, per channel (PRODUCT_REQUIREMENTS.md §10,
    // extended by SPEC_OUTPUT_ROUTING.md):
    //   channelOut = channelIn * gain * (mute ? 0 : 1)
    //                          * ((soloActive && !solo && !exemptFromSolo) ? 0 : 1)
    //   pair[N]    = sum(channelOut over every channel routed to pair N)
    //   pair[0]   *= masterGain   -- Master is deliberately scoped to the
    //     Main pair ONLY, not every pair: the whole point of routing (e.g.
    //     click to a separate IEM output) is that it plays at ITS OWN
    //     level, independent of whatever the Main/house mix is doing --
    //     exactly like a physical desk's Master fader never touches a
    //     Cue/Aux send. A channel routed to any pair other than 0 is
    //     therefore NOT scaled by Master, by design, not by omission.
    void mixDown (const std::array<const float*, kNumMixerChannels>& inL,
                  const std::array<const float*, kNumMixerChannels>& inR,
                  const std::vector<float*>& outLPerPair,
                  const std::vector<float*>& outRPerPair,
                  int numSamples)
    {
        const int numPairs = (int) std::min (outLPerPair.size(), outRPerPair.size());
        if (numPairs <= 0) return;

        for (int p = 0; p < numPairs; ++p)
            for (int i = 0; i < numSamples; ++i) { outLPerPair[(size_t) p][i] = 0.0f; outRPerPair[(size_t) p][i] = 0.0f; }

        bool soloActive = false;
        for (int c = 0; c < kNumMixerChannels; ++c)
            if (channels[(size_t) c].solo.load (std::memory_order_relaxed)) { soloActive = true; break; }

        // snapshot each channel's gain/mute/solo/outputPair ONCE per block
        // (not once per sample) -- avoids kNumMixerChannels*numSamples
        // atomic loads, and matches how a control changing mid-block is
        // expected to behave (takes effect at the next block, same
        // granularity Session's own scheduling already works in whole-chunk
        // terms). outputPair is clamped to [0, numPairs) here, defensively
        // -- see setChannelOutputPair()'s own comment on why the caller's
        // own clamp isn't trusted alone.
        // A mono route writes one side of its pair (side 0/1); stereo writes
        // both (side -1). A mono output the device doesn't have falls back to
        // Main in stereo, rather than to half of some other pair.
        struct Snapshot { float gain; bool silenced; int pair; int side; };
        std::array<Snapshot, kNumMixerChannels> snap {};
        for (int c = 0; c < kNumMixerChannels; ++c)
        {
            auto& ch = channels[(size_t) c];
            const bool muted        = ch.mute.load (std::memory_order_relaxed);
            const bool soloed       = ch.solo.load (std::memory_order_relaxed);
            const bool silencedBySolo = soloActive && ! soloed && ! ch.exemptFromSolo;
            const int  wantedPair = ch.outputPair.load (std::memory_order_relaxed);
            int        side       = ch.outputSide.load (std::memory_order_relaxed);
            int        pair       = std::clamp (wantedPair, 0, numPairs - 1);
            if (side >= 0 && pair != wantedPair) { pair = 0; side = -1; }
            snap[(size_t) c] = { ch.gain.load (std::memory_order_relaxed), muted || silencedBySolo, pair, side };
        }

        const float master = masterGain.load (std::memory_order_relaxed);

        // per-channel running sums for metering, computed alongside the mix
        // itself (no separate pass, no allocation) -- post-fader, post-mute,
        // post-solo, same as before this change; unaffected by which pair a
        // channel routes to, since these sums are taken from `sampL`/
        // `sampR` themselves, before that value is added into any pair.
        std::array<double, kNumMixerChannels> sumSq {};
        std::array<float,  kNumMixerChannels> peakThisBlock {};

        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < kNumMixerChannels; ++c)
            {
                if (snap[(size_t) c].silenced) continue;
                const float sampL = inL[(size_t) c][i] * snap[(size_t) c].gain;
                const float sampR = inR[(size_t) c][i] * snap[(size_t) c].gain;
                const int pair = snap[(size_t) c].pair;
                const int side = snap[(size_t) c].side;
                if (side < 0)
                {
                    outLPerPair[(size_t) pair][i] += sampL;
                    outRPerPair[(size_t) pair][i] += sampR;
                }
                else
                {
                    // mono: both sides at half, so a centred signal keeps its level
                    const float mono = 0.5f * (sampL + sampR);
                    (side == 0 ? outLPerPair : outRPerPair)[(size_t) pair][i] += mono;
                }

                sumSq[(size_t) c] += (double) sampL * sampL + (double) sampR * sampR;
                peakThisBlock[(size_t) c] = std::max (peakThisBlock[(size_t) c], std::max (std::fabs (sampL), std::fabs (sampR)));
            }
        }

        // Master gain: pair 0 (Main) only -- see this method's own header
        // comment for why every other pair intentionally bypasses it.
        for (int i = 0; i < numSamples; ++i)
        {
            outLPerPair[0][i] *= master;
            outRPerPair[0][i] *= master;
        }

        if (numSamples > 0)
        {
            for (int c = 0; c < kNumMixerChannels; ++c)
            {
                const float rms = (float) std::sqrt (sumSq[(size_t) c] / (double) (numSamples * 2));
                channels[(size_t) c].instantRms.store (rms, std::memory_order_relaxed);
                channels[(size_t) c].instantPeak.store (peakThisBlock[(size_t) c], std::memory_order_relaxed);
            }
        }
    }

    // Message thread only, called at the UI's own refresh rate (NOT the
    // audio callback rate -- see this file's header comment on what "frame"
    // means here). Decays this channel's peak-hold state by kPeakHoldDecay
    // and returns PRODUCT_REQUIREMENTS.md §10's own perceptual meter level.
    float updateAndGetMeterLevel (MixerChannel ch)
    {
        auto& c = channels[idx (ch)];
        const float rms  = c.instantRms.load (std::memory_order_relaxed);
        const float peak = c.instantPeak.load (std::memory_order_relaxed);
        c.peakHold = std::max ((double) peak, c.peakHold * kPeakHoldDecay);
        const double level = std::min (1.0, std::sqrt ((double) rms) * 1.35 + c.peakHold * 0.5);
        return (float) level;
    }

private:
    static size_t idx (MixerChannel ch) { return (size_t) ch; }
    static constexpr double kPeakHoldDecay = 0.86;   // PRODUCT_REQUIREMENTS.md §10's own stated decay rate

    struct ChannelState
    {
        std::atomic<float> gain { 1.0f };
        std::atomic<bool>  mute { false };
        std::atomic<bool>  solo { false };
        bool               exemptFromSolo { false };   // write-once, at construction; never mutated at runtime

        // SPEC_OUTPUT_ROUTING.md: 0 = Main/Out 1-2 (every channel's default,
        // identical to this class's pre-routing behavior), 1+ = Out 3-4,
        // 5-6, etc. See setChannelOutputPair()'s own comment.
        std::atomic<int> outputPair { 0 };
        std::atomic<int> outputSide { -1 };   // -1 stereo; 0/1 mono to that side of outputPair

        // Metering: instantRms/instantPeak are audio-thread-written,
        // message-thread-read (atomics); peakHold is message-thread-owned
        // only (touched exclusively by updateAndGetMeterLevel()).
        std::atomic<float> instantRms  { 0.0f };
        std::atomic<float> instantPeak { 0.0f };
        double             peakHold    { 0.0 };
    };

    std::array<ChannelState, kNumMixerChannels> channels;
    std::atomic<float> masterGain { 1.0f };
};

} // namespace ezdeck
