// ============================================================================
//  Deck.h — sample-locked, multi-layer playback engine.
//
//  Pure C++, no JUCE: a "deck" is N layers of pre-decoded audio, all driven by
//  ONE shared playhead. Every layer reads its position as
//  fmod(playhead, layerLength) — so no matter how the layers differ in length,
//  every layer's position is derived from the exact same elapsed-sample count.
//  That's what "phase-aligned" means here: not "same buffer index", but
//  "same point in the shared clock", even if clip lengths differ.
//
//  Kept JUCE-free for the same reason as EzDSP.h: the render loop is the part
//  that must behave identically in the standalone app and the future VST, so
//  it has to be testable without an audio device — see decktest.cpp.
//
//  Assumption: all loaded layers share one native sample rate (true for stems
//  of the same track). The app computes ONE rateRatio (file rate / device
//  rate) and every layer advances through this single shared playhead at
//  that same rate — that's what keeps them locked instead of drifting.
//
//  A layer loops at its own loopLength(): regionLength if the caller has set
//  one (e.g. a tempo-derived 4-bar region), otherwise the full raw buffer.
//  The fmod-based phase lock above is unaffected either way — it only cares
//  what length it's locking to, not where that length came from.
//
//  A layer also carries its own gain and an optional linear fade-in/fade-out,
//  applied at the start/end of its loop region (not the raw buffer). Both
//  default to a no-op (gain 1.0, no fade), so existing callers are unaffected.
// ============================================================================
#pragma once
#include <vector>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>

namespace ezdeck
{

// PX-B: 4 -> 8. Layers 5-8 are ordinary stem layers as far as this engine
// is concerned; the MODE that makes one of them a live input or a hosted
// instrument lives above the engine, in the application, exactly like
// SignatureManager does. Deck.h stays JUCE-free and knows only samples.
constexpr int kNumLayers = 8;

// PLAN_ARRANGEMENT_VIEW.md M1: stem mode's hard cap -- still tempo-locked,
// still griddable, a long loop rather than a freeform recording (the plan's
// own §2 scope fence).
// Owner: whole songs are loaded as stems (a 5-minute song at 70 BPM is ~90
// bars), so the cap is a sanity limit against absurd files, not a musical
// one: 600 bars is 40 minutes at 60 BPM.
constexpr int kMaxStemBars = 600;

// PLAN_ARRANGEMENT_VIEW.md M1: the result of resolving a stem-mode layer's
// real bar length -- `bars` is always the true detected count (even when
// rejected, so a caller can report exactly how far over the cap a file was);
// `accepted` is false once `bars` exceeds the cap, at which point the file
// must not be loaded. Mirrors EzDSP.h's own FitResult-style small value
// struct.
struct StemBarFit
{
    int  bars     { 0 };
    bool accepted { false };
};

// Resolves a stem-mode layer's real bar length from its full duration and
// detected tempo, capped at maxBars. `accepted` is false if the true length
// exceeds maxBars -- import must reject the file, never silently truncate it.
//
// Deliberately NOT in EzDSP.h (out of scope for this milestone -- EzDSP.h
// itself is untouched) and deliberately NOT using ezdsp::AnalysisResult's own
// `bars` field: that field is derived from analyze()'s internal 45-second
// analysis window (EzDSP.h's `maxSec`), which is fine for loop mode's short
// clips but wrong for stem material -- a realistic 16-32 bar file at a
// normal tempo routinely runs past 45 seconds, and analyze().bars would
// silently under-report it. bpm alone is a stable, window-independent
// estimate, so the real bar count is recomputed here from bpm x the FULL
// duration -- the same beatsAt()-style arithmetic EzDSP.h's own fitFourBars
// already uses internally, just not capped to 45 seconds.
//
// Pure arithmetic, no engine state: testable in isolation (stemlentest.cpp)
// exactly like fitFourBars is tested via dsptest.cpp.
inline StemBarFit resolveStemBarLength (double durationSeconds, double bpm, int maxBars = kMaxStemBars)
{
    const double safeBpm = bpm > 0.0 ? bpm : 120.0;
    const double totalBeats = durationSeconds / (60.0 / safeBpm);
    StemBarFit fit;
    fit.bars = std::max (1, (int) std::llround (totalBeats / 4.0));
    fit.accepted = fit.bars <= maxBars;
    return fit;
}

struct Layer
{
    std::vector<float> left;
    std::vector<float> right;      // empty for mono files -> right duplicates left
    bool               loaded        { false };
    std::atomic<bool>  enabled       { true };
    // Owner: "I should be able to zoom into the waveform ... in case I want
    // to change the start and end markers." Until this there was only an END
    // (regionLength); the region always began at sample 0, which the editor's
    // own header comment called out as a known gap. regionStart is the first
    // sample of the playable region; regionLength stays a LENGTH measured
    // FROM regionStart, so every existing project (start 0) loads and plays
    // byte-identically.
    int                regionStart   { 0 };      // first playable sample; 0 = start of the buffer
    int                regionLength  { 0 };      // 0 = unset -> to the end of the buffer
    float              gain          { 1.0f };   // per-layer volume, multiplies output
    int                fadeInSamples { 0 };      // 0 = no fade-in
    int                fadeOutSamples{ 0 };      // 0 = no fade-out
    bool               trimmed       { false };  // true once a user has deliberately set a
                                                  // custom region -- wired to loadLayer's
                                                  // reload guard, see project/MILESTONE_3_EXECUTION_PLAN.md M3-T3

    // PLAN_ARRANGEMENT_VIEW.md M4: live mute/unmute during stem playthrough,
    // click-free. Unlike fadeInSamples/fadeOutSamples above (a pure function
    // of playback position, fine for a fixed region boundary), a mute
    // toggle can land at any arbitrary sample, so the ramp needs its own
    // persisted state -- muteGain is that state, updated once per sample on
    // the audio thread only (never touched cross-thread; only `enabled`,
    // already atomic, is). muteFadeSamples == 0 (every caller today) makes
    // muteGain snap straight to 0/1 every sample -- i.e. the exact old
    // hard-cut gate, byte-for-byte -- so no existing caller's output changes
    // unless it explicitly opts in by setting muteFadeSamples > 0. Mirrors
    // fadeInSamples/fadeOutSamples' own "0 = no-op" convention.
    int                muteFadeSamples { 0 };
    float              muteGain        { 1.0f };

    // PLAN_ARRANGEMENT_VIEW.md M1: the real, detected bar length of a
    // stem-mode layer (up to kMaxStemBars), set at import time by
    // Main.cpp's loadLayer() via resolveStemBarLength() below. 0 = unset --
    // loop-mode layers never populate this; it plays no part in loop-mode
    // playback or in Deck::render()/renderPerTab(), which stay exactly as
    // they were. Purely descriptive this milestone (no timeline, no
    // playback change) -- read by the Arrangement View starting M5+.
    int                stemBarLength { 0 };

    // Milestone 4 (M4-T2): cross-thread buffer-swap primitive, resolving
    // ARCHITECTURE.md's Architecture Decision Pending #2. `pendingSwapReady`
    // gates the other three fields exactly like `enabled` above -- the
    // message thread always finishes writing pendingLeft/pendingRight/
    // pendingRegionLength BEFORE setting pendingSwapReady true, and the
    // audio thread only reads them after observing it true, so it never
    // sees a partially-staged swap. At most one pending swap per layer at a
    // time -- stagePendingSwap() returns false and does nothing if one is
    // already staged and not yet consumed.
    std::atomic<bool>  pendingSwapReady { false };
    std::vector<float> pendingLeft;
    std::vector<float> pendingRight;
    int                pendingRegionLength { 0 };

    int numFrames() const { return (int) left.size(); }

    // The length Deck actually loops this layer against: regionLength if set
    // (e.g. a tempo-derived 4-bar region), otherwise the full raw buffer.
    // Clamped to numFrames() -- regionLength is written by several call
    // sites (loadLayer's fitFourBars fit, the clip editor's manual
    // region-end drag, and the tempo re-warp pipeline's own
    // ezdsp::reWarpLayer/fitFourBars composition), and a confirmed live
    // reproduction found regionLength holding a stale value describing a
    // buffer LARGER than the layer's current one after a re-warp -- this is
    // the single authoritative accessor render()/renderPerTab()/phaseOf()
    // all already trust, so clamping here (rather than, or in addition to,
    // each caller) closes the out-of-bounds hole regardless of which
    // upstream write left regionLength stale.
    // First playable sample, always inside the buffer. Every read of
    // regionStart goes through this -- a stale/oversized value can then never
    // index past the buffer, the same guarantee loopLength() below provides
    // for the length (and for the same reason: several call sites write these
    // fields, including across a re-warp buffer swap).
    int regionStartClamped() const
    {
        const int n = numFrames();
        if (n <= 0) return 0;
        return regionStart > 0 ? std::min (regionStart, n - 1) : 0;
    }

    // The length Deck actually loops this layer against, measured FROM
    // regionStartClamped() -- regionLength if set, otherwise everything from
    // the start marker to the end of the buffer. Clamped so start+length can
    // never exceed the buffer.
    int loopLength() const
    {
        const int start = regionStartClamped();
        const int avail = numFrames() - start;
        if (avail <= 0) return 0;
        return regionLength > 0 ? std::min (regionLength, avail) : avail;
    }

    // SPEC_WARP_BUG_INVESTIGATION.md root-cause fix. loopLength()'s own
    // comment above already documents the SYMPTOM (a stale, oversized
    // regionLength after a re-warp); this closes the actual CAUSE, which is
    // project/KNOWN_BUGS.md #13's already-tracked data race extended to a
    // concrete scenario: the clip editor's "Fit 4 Bars" / region-end-drag
    // handlers (Main.cpp) compute a candidate regionLength from a duration
    // they read from `numFrames()` at the START of their own calculation,
    // then write it directly -- with NO coordination against Milestone 4's
    // stagePendingSwap()/applyPendingSwapIfAny() re-warp mechanism, which
    // can install a smaller buffer for the SAME layer in between. Whichever
    // write lands last wins the plain `int` field; when the swap lands
    // FIRST and the clip editor's now-stale (larger) candidate commits
    // SECOND, the stored regionLength ends up describing a buffer that no
    // longer exists -- reproducing exactly the "~45% larger than the
    // layer's actual buffer" symptom the crash investigation found. Neither
    // reWarpLayer() nor fitFourBars() (EzDSP.h/WarpIntegration.h) is at
    // fault -- both already derive their own regionLength from the
    // POST-warp buffer and clamp to it (fitFourBars' own `std::min
    // (duration, len)`), confirmed by direct inspection, not assumed.
    //
    // The fix: every regionLength candidate from a clip-editor-style direct
    // write goes through THIS method instead of a bare assignment, clamping
    // against numFrames() as read AT THE MOMENT OF THE WRITE -- so even if
    // the candidate was computed against a since-superseded buffer size,
    // the value that actually lands can never describe a buffer larger than
    // whatever is truly live right now. 0 keeps its existing "unset -- loop
    // the full current buffer" sentinel meaning unchanged (loopLength()'s
    // own fallback already handles that case correctly with no stored
    // length to go stale).
    void setRegionLengthClamped (int candidateRegionLength)
    {
        const int avail = std::max (1, numFrames() - regionStartClamped());
        regionLength = candidateRegionLength > 0 ? std::min (candidateRegionLength, avail)
                                                  : candidateRegionLength;
    }

    // Owner's start marker. Same clamp-at-the-moment-of-write discipline as
    // setRegionLengthClamped above, plus one extra invariant: the region must
    // keep at least one sample, so a start marker dragged past the end never
    // produces an empty (or negative-length) region for render() to index.
    void setRegionStartClamped (int candidateRegionStart)
    {
        const int n = numFrames();
        if (n <= 0) { regionStart = 0; return; }
        regionStart = std::max (0, std::min (candidateRegionStart, n - 1));
        if (regionLength > 0)
            regionLength = std::min (regionLength, n - regionStart);
    }

    // Message thread only. Stages a re-warped buffer + its newly-fitted
    // region length for later application by the audio thread. Returns
    // false (does nothing) if a previously-staged swap hasn't been consumed
    // yet -- the caller must not overwrite an in-flight staged buffer.
    bool stagePendingSwap (std::vector<float> newLeft, std::vector<float> newRight, int newRegionLength)
    {
        if (pendingSwapReady.load (std::memory_order_acquire)) return false;
        pendingLeft = std::move (newLeft);
        pendingRight = std::move (newRight);
        pendingRegionLength = newRegionLength;
        pendingSwapReady.store (true, std::memory_order_release);
        return true;
    }

    // Audio thread only, called at a safe block boundary (never mid-sample).
    // O(1) and allocation-free whether or not a swap is pending: a single
    // atomic load if not, or three std::vector::swap calls if so --
    // guaranteed O(1)/no-allocation by the standard's own complexity
    // guarantee for vector::swap, not an assumption specific to this codebase.
    void applyPendingSwapIfAny()
    {
        if (! pendingSwapReady.load (std::memory_order_acquire)) return;
        left.swap (pendingLeft);
        right.swap (pendingRight);
        regionLength = pendingRegionLength;
        pendingSwapReady.store (false, std::memory_order_release);
    }
};

// Milestone 5 (M5-T2): a deck plays either in a continuous loop (today's
// only behavior, Milestones 1-4) or as a stem -- full length, once, no
// wraparound. See ARCHITECTURE.md's "Stem mode & crossfade" Extension Point
// and Architecture Decision Pending #8 for how this coexists with
// Session's switching invariants.
enum class DeckMode { loop, stem };

// Only meaningful when mode == DeckMode::stem; PRODUCT_REQUIREMENTS.md §3's
// three end-of-stem behaviors. nextPlay is the PRD's own stated default.
enum class StemEndBehavior { next, nextPlay, loop };

class Deck
{
public:
    std::array<Layer, kNumLayers> layers;

    DeckMode          mode            { DeckMode::loop };   // the app puts every row into stem mode (SessionComponent); the engine's own default stays loop
    StemEndBehavior   stemEndBehavior { StemEndBehavior::nextPlay };

    // Milestone 6 (M6-T2): the structural beat count of whichever signature
    // bank this deck belongs to -- conceptually owned by that signature
    // (ARCHITECTURE.md's resolved Architecture Decision Pending #1), but
    // physically held here so Session::render() never has to query the
    // signature/mapping layer above it. Sentinel 0 = unset -> Session falls
    // back to its own Tempo::beatsPerBar, so every pre-Milestone-6 caller
    // (decktest.cpp, switchtest.cpp, a Deck used with no signature concept
    // at all) is completely unaffected. Written once, at signature
    // configuration time, by SignatureManager -- never by Session itself.
    int beatsPerBar { 0 };

    // Milestone 6: same pattern as beatsPerBar above, for Session::render()'s
    // own internal nextPlay auto-advance (Milestone 5, M5-T3) -- bounds
    // findNextDeckWithContent() to this deck's own signature's contiguous
    // flat-index range, so a stem's natural end never auto-advances into an
    // unrelated signature bank (PRODUCT_REQUIREMENTS.md §1: banks stay
    // independent "without interfering with each other"). Sentinel -1 =
    // unset -> Session searches the whole array, exactly matching
    // findNextDeckWithContent()'s own pre-Milestone-6 default. Written once,
    // at signature configuration time, by SignatureManager.
    int autoAdvanceRangeStart { -1 };
    int autoAdvanceRangeCount { -1 };

    void setRateRatio (double r) { rateRatio = r; }
    double getRateRatio() const  { return rateRatio; }

    void reset() { playhead = 0.0; }
    double playheadPosition() const { return playhead; }

    // UI_SPEC_ARRANGEMENT_M5.md §6.2: a minimal, read-oriented counterpart to
    // playheadPosition() above, for click-to-seek. Sets the ONE shared
    // playhead directly -- every layer already reads it (render()/
    // renderPerTab() above), so all four stay locked to the new position by
    // the same construction that keeps them locked during ordinary playback;
    // nothing per-layer to update. Deliberately mode-agnostic and minimal
    // (just clamps to non-negative) -- bar-boundary snapping and any
    // range/end clamping belong to the caller, which knows about bars/tempo;
    // Deck.h intentionally doesn't. Message-thread only, same as reset().
    void seekTo (double samplePosition) { playhead = std::max (0.0, samplePosition); }

    // ---- section loop (stem mode) -----------------------------------------
    //
    // AbleSet-style "loop this section": when armed, the playhead wraps from
    // loopEnd back to loopStart, sample-exactly, preserving its sub-sample
    // phase so a rateRatio != 1 deck does not drift by a fraction of a
    // sample on every pass. Positions are in the deck's own playhead units
    // (layer samples), the same frame seekTo()/playheadPosition() use; the
    // caller (which knows bars and tempo) converts.
    //
    // remaining: -1 = loop until disarmed; n > 0 = play the section n more
    // times, then fall through and disarm ("+LOOP:n"). Written by the
    // message thread, read by the audio thread; each field is its own atomic
    // and the audio thread snapshots them once per render call, so the worst
    // case of a mid-block change is that it takes effect next block --
    // exactly the granularity every other control here already has.
    void setSectionLoop (double startPos, double endPos, int remaining = -1)
    {
        sectionLoopStart.store (std::max (0.0, startPos), std::memory_order_relaxed);
        sectionLoopEnd.store   (std::max (0.0, endPos),   std::memory_order_relaxed);
        sectionLoopRemaining.store (remaining, std::memory_order_relaxed);
        sectionLoopEnabled.store (endPos > startPos, std::memory_order_release);
    }

    void clearSectionLoop()              { sectionLoopEnabled.store (false, std::memory_order_release); }
    bool isSectionLoopEnabled() const    { return sectionLoopEnabled.load (std::memory_order_acquire); }
    double getSectionLoopStart() const   { return sectionLoopStart.load (std::memory_order_relaxed); }
    double getSectionLoopEnd() const     { return sectionLoopEnd.load (std::memory_order_relaxed); }
    int  getSectionLoopRemaining() const { return sectionLoopRemaining.load (std::memory_order_relaxed); }


    // The deck's own natural, un-looped length in stem mode: the longest
    // LOADED layer's numFrames() (deliberately not gated on `enabled` --
    // toggling a layer's mute mid-playback shouldn't retroactively change
    // when the deck's stem is considered finished). Meaningless in loop
    // mode, where each layer already has its own well-defined loopLength().
    int stemLength() const
    {
        int longest = 0;
        for (auto& layer : layers)
            if (layer.loaded) longest = std::max (longest, layer.numFrames());
        return longest;
    }

    // True once a stem-mode deck's playhead has passed its own stemLength()
    // -- every layer has, by then, individually stopped contributing audio
    // (see render()'s own per-layer cutoff below). Always false in loop
    // mode, which never "finishes" on its own. Session (M5-T3/M5-T4) polls
    // this once per render() chunk to decide whether to apply the deck's
    // stemEndBehavior.
    bool stemFinished() const
    {
        return mode == DeckMode::stem && playhead >= (double) stemLength();
    }

    // Milestone 4 (M4-T2): applies any pending swap for every layer in this
    // deck. Callers decide WHEN it's safe to call this -- see ARCHITECTURE.md's
    // resolved Architecture Decision Pending #2: for an inactive deck, any
    // time is safe (the audio thread never reads an inactive deck's layers);
    // for the active deck, only at a scheduled bar boundary (Session's job,
    // not Deck's -- see project/MILESTONE_4_IMPLEMENTATION_PLAN.md M4-T7).
    void applyPendingSwaps()
    {
        for (auto& layer : layers) layer.applyPendingSwapIfAny();
    }

    // Current read position within a given layer's own buffer, in samples.
    // Exposed so tests (and UI/debug code) can check phase without depending
    // on render()'s internals. Mode-aware since M5-T2: in stem mode there is
    // no wraparound, so this clamps at the layer's own length instead of
    // taking a modulus, matching render()'s own per-layer cutoff exactly.
    // Returns an ABSOLUTE buffer position (region start included), so a UI
    // playhead drawn from this lands on the right place in the waveform.
    double phaseOf (int layerIndex) const
    {
        const auto& layer = layers[(size_t) layerIndex];
        const int   start = layer.regionStartClamped();
        const int   len   = layer.loopLength();
        if (len <= 0) return (double) start;

        if (mode == DeckMode::stem)
            return (double) start + std::min ((double) len, playhead);

        double p = std::fmod (playhead, (double) len);
        if (p < 0.0) p += len;
        return (double) start + p;
    }

    // Renders numSamples of stereo output into outL/outR (each must already be
    // allocated to numSamples). Overwrites — does not accumulate into — the
    // output. Safe to call with any block size; playhead state carries over
    // exactly between calls regardless of how the caller chunks samples.
    void render (float* outL, float* outR, int numSamples, float masterGain = 0.5f)
    {
        // section loop: one snapshot per call (see setSectionLoop()'s comment)
        bool   loopArmed     = mode == DeckMode::stem && sectionLoopEnabled.load (std::memory_order_acquire);
        double loopStart     = sectionLoopStart.load (std::memory_order_relaxed);
        double loopEnd       = sectionLoopEnd.load (std::memory_order_relaxed);
        int    loopRemaining = sectionLoopRemaining.load (std::memory_order_relaxed);
        if (loopEnd <= loopStart) loopArmed = false;
        for (int i = 0; i < numSamples; ++i)
        {
            float mixL = 0.0f, mixR = 0.0f;

            for (auto& layer : layers)
            {
                if (! layer.loaded) continue;

                // PLAN_ARRANGEMENT_VIEW.md M4: update the mute ramp every
                // sample, regardless of whether this layer ends up
                // contributing -- muteGain must keep tracking `enabled` even
                // while fully silent, so a later re-enable ramps up from 0,
                // not from whatever it was mid-fade. See Layer::muteGain's
                // own comment for why this can't reuse fadeIn/fadeOutSamples.
                if (layer.muteFadeSamples > 0)
                {
                    const float step = 1.0f / (float) layer.muteFadeSamples;
                    layer.muteGain = layer.enabled.load (std::memory_order_relaxed)
                                       ? std::min (1.0f, layer.muteGain + step)
                                       : std::max (0.0f, layer.muteGain - step);
                }
                else
                {
                    layer.muteGain = layer.enabled.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
                }

                if (layer.muteGain <= 0.0f) continue;   // fully silent -- identical to the old !enabled gate when muteFadeSamples == 0

                int    len;
                double pos;

                // Owner's start marker: both modes play the region
                // [regionStart, regionStart + loopLength). regionStart is 0
                // for every clip that has never been trimmed at the front, so
                // this is identical to the pre-marker behaviour there.
                const int regionStart = layer.regionStartClamped();

                if (mode == DeckMode::stem)
                {
                    // no wraparound: a layer past its own length simply
                    // stops contributing (silence), rather than looping --
                    // that's what "play once" means. Session (M5-T3/T4)
                    // separately polls stemFinished() to react once every
                    // loaded layer has reached this point.
                    len = layer.loopLength();
                    if (len <= 0 || playhead >= (double) len) continue;
                    pos = (double) regionStart + playhead;
                }
                else
                {
                    len = layer.loopLength();
                    if (len <= 0) continue;
                    double p = std::fmod (playhead, (double) len);
                    if (p < 0.0) p += len;
                    pos = (double) regionStart + p;
                }

                const int   idx0 = (int) pos;
                // The neighbour sample must stay INSIDE the region: stem mode
                // clamps at the region's last sample, loop mode wraps back to
                // regionStart (not to sample 0 -- that would splice the loop
                // point onto the discarded head of the file).
                const int   idx1 = (mode == DeckMode::stem)
                                     ? std::min (regionStart + len - 1, idx0 + 1)
                                     : regionStart + ((idx0 - regionStart + 1) % len);
                const float frac = (float) (pos - idx0);

                // Defence in depth: `len` (loopLength()) is now clamped to
                // numFrames() at the source (see Layer::loopLength()'s own
                // comment for why that alone isn't a complete guarantee --
                // this is a second, independent bound directly against the
                // buffers actually being indexed). Silent clamp, no I/O --
                // real-time-safe, unlike a log call on this thread would be.
                const int idx0L = std::min (idx0, (int) layer.left.size() - 1);
                const int idx1L = std::min (idx1, (int) layer.left.size() - 1);
                const float l0 = layer.left[(size_t) idx0L];
                const float l1 = layer.left[(size_t) idx1L];
                const float l  = l0 + frac * (l1 - l0);

                float r;
                if (! layer.right.empty())
                {
                    const int idx0R = std::min (idx0, (int) layer.right.size() - 1);
                    const int idx1R = std::min (idx1, (int) layer.right.size() - 1);
                    const float r0 = layer.right[(size_t) idx0R];
                    const float r1 = layer.right[(size_t) idx1R];
                    r = r0 + frac * (r1 - r0);
                }
                else
                {
                    r = l;
                }

                // linear fade-in/out across the loop region's own boundaries.
                // Measured from the REGION start, not the buffer start, so a
                // fade still means "the first/last N samples of what plays".
                const double posInRegion = pos - (double) regionStart;
                float envelope = 1.0f;
                if (layer.fadeInSamples > 0 && posInRegion < (double) layer.fadeInSamples)
                    envelope = (float) (posInRegion / (double) layer.fadeInSamples);
                if (layer.fadeOutSamples > 0 && posInRegion > (double) (len - layer.fadeOutSamples))
                {
                    const float outEnvelope = (float) (((double) len - posInRegion) / (double) layer.fadeOutSamples);
                    envelope = std::min (envelope, outEnvelope);
                }

                const float g = layer.gain * envelope * layer.muteGain;
                mixL += l * g;
                mixR += r * g;
            }

            outL[i] = mixL * masterGain;
            outR[i] = mixR * masterGain;
            playhead += rateRatio;
            if (loopArmed && playhead >= loopEnd)
            {
                // Wrap by the loop LENGTH rather than snapping to loopStart,
                // so whatever fraction of a sample we overshot by is carried
                // into the next pass -- the same sub-sample discipline that
                // keeps four layers phase-locked keeps a looped section from
                // drifting against the master clock.
                playhead -= (loopEnd - loopStart);
                if (loopRemaining > 0 && --loopRemaining == 0)
                {
                    loopArmed = false;
                    sectionLoopEnabled.store (false, std::memory_order_release);
                }
                sectionLoopRemaining.store (loopRemaining, std::memory_order_relaxed);
            }
        }
    }

    // Milestone 7 (M7): renders each layer's own contribution into its OWN
    // buffer (outL[layerIdx]/outR[layerIdx]) instead of summing them into one
    // pair -- lets a Mixer route each tab/layer through its own independent
    // channel (PRODUCT_REQUIREMENTS.md §10's "Tabs 1-4" are per LAYER INDEX,
    // not one flat per-deck channel; ARCHITECTURE.md's resolved Architecture
    // Decision Pending #4). Every outL[i]/outR[i] buffer must already be
    // allocated to numSamples by the caller. Reuses render()'s own exact
    // per-sample interpolation/envelope/gain math verbatim -- deliberately a
    // separate, additive method rather than a refactor of render() itself,
    // so render()'s own proven, bit-exact behavior (decktest.cpp) is
    // completely unaffected by this method's existence. No masterGain
    // parameter -- all final gain staging (channel gain x master gain) is
    // the Mixer's job now, not Deck's.
    void renderPerTab (std::array<float*, kNumLayers> outL, std::array<float*, kNumLayers> outR, int numSamples)
    {
        // section loop: one snapshot per call (see setSectionLoop()'s comment)
        bool   loopArmed     = mode == DeckMode::stem && sectionLoopEnabled.load (std::memory_order_acquire);
        double loopStart     = sectionLoopStart.load (std::memory_order_relaxed);
        double loopEnd       = sectionLoopEnd.load (std::memory_order_relaxed);
        int    loopRemaining = sectionLoopRemaining.load (std::memory_order_relaxed);
        if (loopEnd <= loopStart) loopArmed = false;
        for (int i = 0; i < numSamples; ++i)
        {
            for (size_t li = 0; li < (size_t) kNumLayers; ++li)
            {
                auto& layer = layers[li];
                float sampleL = 0.0f, sampleR = 0.0f;

                if (layer.loaded)
                {
                    // PLAN_ARRANGEMENT_VIEW.md M4: see render()'s identical
                    // block for why this runs unconditionally (every sample,
                    // regardless of whether the layer ends up silent) and
                    // why muteFadeSamples == 0 reproduces the old !enabled
                    // gate byte-for-byte.
                    if (layer.muteFadeSamples > 0)
                    {
                        const float step = 1.0f / (float) layer.muteFadeSamples;
                        layer.muteGain = layer.enabled.load (std::memory_order_relaxed)
                                           ? std::min (1.0f, layer.muteGain + step)
                                           : std::max (0.0f, layer.muteGain - step);
                    }
                    else
                    {
                        layer.muteGain = layer.enabled.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
                    }

                    int    len;
                    double pos;
                    bool   silent = layer.muteGain <= 0.0f;

                    // owner's start marker -- see render()'s identical block
                    const int regionStart = layer.regionStartClamped();

                    if (mode == DeckMode::stem)
                    {
                        len = layer.loopLength();
                        if (len <= 0 || playhead >= (double) len) silent = true;
                        pos = (double) regionStart + playhead;
                    }
                    else
                    {
                        len = layer.loopLength();
                        // len <= 0 leaves pos untouched below -- fmod(x, 0) is
                        // NaN and raises FE_INVALID, so it must not run at all
                        if (len <= 0) { silent = true; pos = (double) regionStart; }
                        else
                        {
                            double p = std::fmod (playhead, (double) len);
                            if (p < 0.0) p += len;
                            pos = (double) regionStart + p;
                        }
                    }

                    if (! silent)
                    {
                        const int   idx0 = (int) pos;
                        const int   idx1 = (mode == DeckMode::stem)
                                             ? std::min (regionStart + len - 1, idx0 + 1)
                                             : regionStart + ((idx0 - regionStart + 1) % len);
                        const float frac = (float) (pos - idx0);

                        // Defence in depth -- see render()'s identical block
                        // for why. Silent clamp, no I/O -- real-time-safe.
                        const int idx0L = std::min (idx0, (int) layer.left.size() - 1);
                        const int idx1L = std::min (idx1, (int) layer.left.size() - 1);
                        const float l0 = layer.left[(size_t) idx0L];
                        const float l1 = layer.left[(size_t) idx1L];
                        const float l  = l0 + frac * (l1 - l0);

                        float r;
                        if (! layer.right.empty())
                        {
                            const int idx0R = std::min (idx0, (int) layer.right.size() - 1);
                            const int idx1R = std::min (idx1, (int) layer.right.size() - 1);
                            const float r0 = layer.right[(size_t) idx0R];
                            const float r1 = layer.right[(size_t) idx1R];
                            r = r0 + frac * (r1 - r0);
                        }
                        else
                        {
                            r = l;
                        }

                        // fades measured from the REGION start -- see render()
                        const double posInRegion = pos - (double) regionStart;
                        float envelope = 1.0f;
                        if (layer.fadeInSamples > 0 && posInRegion < (double) layer.fadeInSamples)
                            envelope = (float) (posInRegion / (double) layer.fadeInSamples);
                        if (layer.fadeOutSamples > 0 && posInRegion > (double) (len - layer.fadeOutSamples))
                        {
                            const float outEnvelope = (float) (((double) len - posInRegion) / (double) layer.fadeOutSamples);
                            envelope = std::min (envelope, outEnvelope);
                        }

                        const float g = layer.gain * envelope * layer.muteGain;
                        sampleL = l * g;
                        sampleR = r * g;
                    }
                }

                outL[li][i] = sampleL;
                outR[li][i] = sampleR;
            }

            playhead += rateRatio;
            if (loopArmed && playhead >= loopEnd)
            {
                // Wrap by the loop LENGTH rather than snapping to loopStart,
                // so whatever fraction of a sample we overshot by is carried
                // into the next pass -- the same sub-sample discipline that
                // keeps four layers phase-locked keeps a looped section from
                // drifting against the master clock.
                playhead -= (loopEnd - loopStart);
                if (loopRemaining > 0 && --loopRemaining == 0)
                {
                    loopArmed = false;
                    sectionLoopEnabled.store (false, std::memory_order_release);
                }
                sectionLoopRemaining.store (loopRemaining, std::memory_order_relaxed);
            }
        }
    }

private:
    double playhead  { 0.0 };
    double rateRatio { 1.0 };

    std::atomic<bool>   sectionLoopEnabled   { false };
    std::atomic<double> sectionLoopStart     { 0.0 };
    std::atomic<double> sectionLoopEnd       { 0.0 };
    std::atomic<int>    sectionLoopRemaining { -1 };
};

} // namespace ezdeck
