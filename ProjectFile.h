// ============================================================================
//  ProjectFile.h — the on-disk project format (Milestone 12,
//  PRODUCT_REQUIREMENTS.md §13; resolves ARCHITECTURE.md's Architecture
//  Decision Pending #5).
//
//  JUCE-side (uses juce::String/juce::var/juce::File/juce::JSON — already
//  part of JUCE, no new external dependency), but deliberately depends on
//  ONLY juce_core, not juce_gui_basics/juce_audio_utils/AudioAppComponent --
//  every plain-data snapshot struct here and its toVar()/fromVar() functions
//  can be exercised without a running GUI application, which is exactly what
//  EzPlayApplication's `--selftest-persistence` command-line mode uses to
//  verify the JSON round-trip automatically (see Main.cpp).
//
//  Session.h/Deck.h/Mixer.h/OneShotVoice.h/Metronome.h stay JUCE-free exactly
//  as established -- SessionComponent (Main.cpp) is the only place that
//  gathers a ProjectSnapshot from its own live members and scatters one back
//  into them; this file only ever converts a ProjectSnapshot to/from JSON.
//
//  Object identity: no clip-ID/library system exists yet (Milestone 16) --
//  a clip's identity here is its slot position + the file path it was loaded
//  from, an honest, documented simplification of PRD §13's fuller
//  content-addressed model (project/MILESTONE_11-15_ARCHITECTURE.md).
// ============================================================================
#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>

namespace ezproject
{

constexpr int kCurrentVersion = 1;

struct LayerSnapshot
{
    juce::String filePath;
    // Milestone 16: optional Library reference (ARCHITECTURE.md's resolved
    // Architecture Decision Pending #9). Empty (every project saved before
    // Milestone 16) preserves the exact pre-Milestone-16 behavior -- filePath
    // is used literally, unchanged. When present, the loader resolves
    // through LibraryManager first (surviving a moved/renamed file) and
    // falls back to filePath only if that resolution fails. Purely
    // additive -- no version bump needed (Milestone 12's own resolved
    // versioning strategy: bump only on a real breaking change).
    juce::String assetId;
    // Owner's start marker (Deck.h Layer::regionStart). Additive -- absent in
    // every project saved before it existed, which reads as 0 = "region
    // starts at the beginning", i.e. exactly the old behaviour.
    int   regionStart    { 0 };
    int   regionLength   { 0 };
    float gain           { 1.0f };
    int   fadeInSamples  { 0 };
    int   fadeOutSamples { 0 };
    bool  trimmed        { false };
    bool  enabled        { true };

    // SPEC_PERFORM_V2 GROUP B: "name & colour are slot-persistent -- stored
    // on the slot, survive loop swaps and save/load." Additive fields, same
    // "no version bump for a purely-additive field" policy this file's own
    // header comment already established for assetId above.
    juce::String nameOverride;
    bool         colourSet   { false };
    juce::uint32 colourArgb  { 0 };

    // SPEC_PERFORM_V2 GROUP H4: "a loop can be linked to one pad." -1 = no
    // link. Additive, same policy as every other purely-additive field.
    int linkedPad { -1 };
};

struct DeckSnapshot
{
    int flatIndex { 0 };
    // PX-B: 8, matching ezdeck::kNumLayers. This array is indexed by loops
    // bounded by that engine constant, so leaving it at 4 was an
    // out-of-bounds READ the moment the engine grew -- which is exactly how
    // it first showed up: a hang on startup, not a crash, because the
    // garbage it read was a juce::String header.
    static constexpr int kDeckLayers = 8;
    std::array<LayerSnapshot, kDeckLayers> layers;
    double tempoOverrideBpm { -1.0 };   // -1 = no override ("play as recorded")
    // Owner: one tempo per song, set by the user, never detected. -1 = not
    // set yet (nothing is ever stretched until it is). Additive field.
    double sourceBpm { -1.0 };
    bool   stemMode  { true };          // ezdeck::DeckMode; absent reads as stem (the default)
    // The song's own click and guide tracks, beside the row (not on a deck).
    // Paths are validated on load like every layer path. Additive.
    juce::String clickFile, guideFile;
    bool useSongClick { true }, useSongGuide { true };   // the song's own track, or the built-in click/cues
    juce::String meter;   // the song's own time signature ("6/8"); "" = its signature group's. Additive.

    // Phase 1.1 P1 "Row Management" -- purely descriptive per-row metadata
    // (Main.cpp's own comment on layerDisplayName() explains the Row=deck
    // mapping). Additive fields, same "bump only on a real breaking change"
    // policy as assetId's own addition -- no version bump needed. Inherits
    // the same "only decks with at least one loaded layer get a snapshot"
    // limitation tempoOverrideBpm above has always had: naming/coloring an
    // empty row before loading anything into it doesn't survive a
    // save/reload. Flagged, not silently accepted as new scope creep to fix.
    juce::String rowName;
    bool         rowColourSet { false };
    juce::uint32 rowColourArgb { 0 };
    juce::String rowNotes;
    juce::String rowTags;

    // Section playback (Arrangement.h): the song's sections in bars, plus
    // end-of-song and count-in policy. Additive, no version bump -- an
    // absent "arrangement" loads as "no sections", which is exactly what
    // every project saved before this feature had.
    struct SectionSnapshot
    {
        juce::String name;
        int          startBar { 0 };
        juce::uint32 colourArgb { 0 };
        bool skip { false }, optional { false }, loopOnEntry { false }, pauseAfter { false };
        juce::String cue;   // Guide.h: "" automatic, "-" silent, else a cue stem
    };
    std::vector<SectionSnapshot> sections;
    int  arrangementLengthBars { 0 };
    int  endBehaviour { 2 };        // 0 stop, 1 cue next, 2 auto-advance, 3 next with count-in, 4 fade into next, 5 repeat (ezarr::EndBehaviour)
    int  endFadeSeconds { 2 };      // for 4: 1, 2 or 4 seconds
    int  countInBars { 0 };

    // Native guide tracks (Guide.h). Additive; absent reads as off.
    bool guideClick { false };
    bool guideCues { false };
    int  cueLeadBars { 2 };
    bool cueCounts { true };
};

struct VoiceSnapshot
{
    int index { 0 };
    juce::String filePath;
    juce::String assetId;   // Milestone 16 -- see LayerSnapshot's own comment
    bool  loop { false };
    float gain { 1.0f };

    // PerformLive UI/UX Design Notes (Studio One reference): "each pad
    // supports Name/Color/.../Mute/Solo" -- same additive-field policy as
    // LayerSnapshot's own nameOverride/colourSet/colourArgb above (no
    // version bump). enabled defaults true (unmuted) so pre-existing
    // projects with no such field load exactly as before.
    bool         enabled      { true };
    bool         soloed       { false };
    juce::String nameOverride;
    bool         colourSet    { false };
    juce::uint32 colourArgb   { 0 };
};

struct MixerChannelSnapshot
{
    float gain { 1.0f };
    bool  mute { false };
    bool  solo { false };
    // Where this channel plays (Mixer.h's route codes, ezdeck::outputTargetFor):
    // 0 = Main (Out 1/2, through Master); 1..kMaxOutputRoutes-1 = hardware
    // stereo pair N, straight to those jacks; kMonoRouteBase + c = hardware
    // output c+1 alone (mono). Additive field, no version bump.
    static constexpr int kMaxOutputRoutes    = 32;    // == ezdeck::kMaxStereoRoutes
    static constexpr int kMonoRouteBase      = 100;   // == ezdeck::kMonoRouteBase
    static constexpr int kMaxOutputChannels  = 64;    // == ezdeck::kMaxOutputChannels
    int outputRoute { 0 };

    /** A stored route made safe: a valid code is kept; a pair past the end is
        the last pair (as before mono existed); anything else is Main. */
    static int sanitiseRoute (int code)
    {
        if (code >= 0 && code < kMaxOutputRoutes) return code;
        if (code >= kMonoRouteBase && code < kMonoRouteBase + kMaxOutputChannels) return code;
        if (code >= kMaxOutputRoutes && code < kMonoRouteBase) return kMaxOutputRoutes - 1;
        return 0;
    }
};

struct SceneSnapshot
{
    bool         filled { false };
    juce::String name;
    int          signatureIndex { 0 };
    int          activeSlot     { 0 };
    double       bpm { 120.0 };
    // PX-B: 8, matching ezdeck::kNumLayers. Deliberately a literal rather
    // than the engine constant -- this header is the on-disk schema and must
    // not start tracking an engine value that could silently rewrite what
    // old files mean. kSceneTabs is asserted against kNumLayers in Main.cpp.
    static constexpr int kSceneTabs = 8;
    std::array<bool, kSceneTabs> tabEnabled { true, true, true, true,
                                              true, true, true, true };
    std::array<bool, 12> padActive {};

    // SPEC_PERFORM_V2 GROUP E: owner-assignable scene colour ("lights up
    // like a MIDI keyboard"). Additive, same policy as every other
    // purely-additive field in this file -- no version bump.
    bool         colourSet  { false };
    juce::uint32 colourArgb { 0 };
};

struct SettingsSnapshot
{
    bool metronomeEnabled { false };   // owner: metronome OFF by default
    bool onePadAtATime    { true };
    bool meterVisible     { true };
    bool tempoLockEnabled { false };

    // LEGACY (read only): live input and instrument per deck column, from
    // before the live tracks existed. settingsFromVar() moves them onto the
    // live tracks below; nothing writes them any more.
    static constexpr int kLiveColumns = 8;
    std::array<int,  kLiveColumns> liveInputChannel { -1, -1, -1, -1, -1, -1, -1, -1 };
    std::array<bool, kLiveColumns> liveInputStereo  {};

    // Instrument plugin per column (PX-D): the plugin's identifier string
    // (empty = none) and its saved state, base64. Additive.
    std::array<juce::String, kLiveColumns> instrumentId;
    std::array<juce::String, kLiveColumns> instrumentState;

    // The live tracks (LIVE 1-4 on the mixer): each is a device input
    // (trackInput >= 0, stereo takes the next channel too), an instrument
    // plugin (identifier + base64 state), or nothing. trackMidiChannel: 0 =
    // every channel, else 1-16. Global, not per song: a mic jack is a
    // physical thing.
    static constexpr int kLiveTracks = 4;
    std::array<int,  kLiveTracks>          trackInput { -1, -1, -1, -1 };
    std::array<bool, kLiveTracks>          trackStereo {};
    std::array<juce::String, kLiveTracks>  trackInstrumentId;
    std::array<juce::String, kLiveTracks>  trackInstrumentState;
    std::array<int,  kLiveTracks>          trackMidiChannel {};

    // PERFORM LIVE channel strip per track: 0-7 the decks, 8-11 the live
    // tracks. On/off and full state (every knob, the preset), base64.
    // Files from before the live tracks have 8 entries, which keep meaning
    // the same decks.
    static constexpr int kStrips = kLiveColumns + kLiveTracks;
    std::array<bool, kStrips>         stripOn {};
    std::array<juce::String, kStrips> stripState;

    // The mixer's WEB strip: the browser page's level (0..1) and mute, which
    // also set the LIBRARY preview's level; webRoute is where that preview
    // plays (a MixerChannelSnapshot route code). Additive.
    float webGain { 1.0f };
    bool  webMute { false };
    int   webRoute { 0 };
};

struct ProjectSnapshot
{
    int version { kCurrentVersion };
    SettingsSnapshot settings;
    double masterTempoBpm { 120.0 };
    int    viewedSignature { 0 };
    // One entry per mixer channel, in MixerChannel order: Tab1-8, Live1-4,
    // Pads, Fx, Metro (click), Cues. Asserted against ezdeck::kNumMixerChannels
    // in Main.cpp. Files saved with 7, 11 or 12 entries are remapped on load.
    static constexpr int kMixerChannels = 16;
    std::array<MixerChannelSnapshot, kMixerChannels> mixerChannels;
    float  masterGain { 1.0f };
    std::vector<DeckSnapshot>  decks;   // only decks with at least one loaded layer
    std::vector<VoiceSnapshot> pads;    // only loaded pads
    std::vector<VoiceSnapshot> fx;      // only loaded FX slots
    std::array<SceneSnapshot, 8> scenes;

    // Section playback: the SETLIST is an ordered list of flat deck indices
    // (songs), and the jump mode the performer chose. Additive.
    std::vector<int> setlist;
    int  jumpMode { 1 };            // 0 next bar, 1 end of section, 2 immediately

    // SPEC_PERFORM_V2 GROUP E: owner-assignable time-signature colour ("same
    // treatment [as scenes] -- lit/glowing when active"). Sized 7 to match
    // ezdeck::SignatureManager::kNumSignatures -- a literal, not a reference
    // to that header, since this file deliberately depends on ONLY
    // juce_core (this file's own header comment) and never includes
    // SignatureManager.h. Additive, no version bump.
    std::array<bool,         7> signatureColourSet  {};
    std::array<juce::uint32, 7> signatureColourArgb {};
};

juce::var toVar (const ProjectSnapshot& snapshot);

// Populates `out` from `v`. Returns false (leaving `out` at its
// default-constructed values) if `v` isn't a well-formed project object or
// its version doesn't match kCurrentVersion -- a version mismatch falls back
// to defaults with a clear caller-visible failure rather than guessing at a
// migration (there is nothing to migrate FROM yet; this hook exists for
// whenever a version 2 does).
bool fromVar (const juce::var& v, ProjectSnapshot& out);

bool saveToFile (const ProjectSnapshot& snapshot, const juce::File& file);

// Returns false (leaving `out` untouched) if the file doesn't exist, can't
// be read, or fails to parse -- callers should fall back to their own
// hardcoded defaults in that case, exactly as they did before this
// milestone, not treat it as fatal.
bool loadFromFile (const juce::File& file, ProjectSnapshot& out);

} // namespace ezproject
