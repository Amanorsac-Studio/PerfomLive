#include "ProjectFile.h"
#include <cmath>

namespace ezproject
{

// ============================================================================
//  SECURITY: this file is the SINGLE VALIDATION CHOKE POINT for project data.
//
//  A .perform file is UNTRUSTED input -- users trade projects and packs, so
//  its contents must be treated as attacker-controlled, exactly like a
//  downloaded document. Everything downstream (SessionComponent::
//  applySnapshot and friends) is a mechanical copy into fixed-size arrays
//  and live audio state; it is NOT the place to discover that a JSON int is
//  a 700-million-element array index or that a gain is 1e9.
//
//  So every scalar parsed here is range-clamped to what its CONSUMER can
//  survive, and every collection is capped:
//    - indices  -> clamped/rejected against the destination array's size
//    - gains    -> finite and within a sane multiplier (no speaker-destroying
//                  or NaN values reaching the audio device)
//    - tempos   -> finite and > 0 (a 0/NaN bpm divides by zero in
//                  Session::barLengthSamples, whose llround(inf) is UB on the
//                  AUDIO thread)
//    - arrays   -> hard element caps (a 10M-entry "pads" array is a memory-
//                  exhaustion DoS and nothing beyond index 11 is even used)
//  Rejected values fall back to the struct's own default rather than failing
//  the whole load: a damaged project should open degraded, not not-at-all.
// ============================================================================
namespace
{
    // Sized generously vs. what the app can actually address (56 decks, 12
    // pads/FX, 8 scenes) -- large enough that no legitimate project is ever
    // refused, small enough that a hostile file cannot exhaust memory.
    constexpr int kMaxDecks  = 4096;
    constexpr int kMaxVoices = 4096;

    // Gain: 4.0 = +12 dB, well above any musical use of these controls and
    // far below "instant full-scale square wave into headphones".
    constexpr float kMaxGain = 4.0f;

    bool isFinite (double d) { return std::isfinite (d); }

    float clampGain (const juce::var& v, float fallback)
    {
        const double d = (double) v;
        if (! isFinite (d)) return fallback;
        return juce::jlimit (0.0f, kMaxGain, (float) d);
    }

    // Non-negative sample counts, capped at a length no real clip reaches
    // (~12.6 hours at 44.1kHz) so nothing downstream can overflow an int
    // when multiplied by a channel count or a sample rate.
    int clampSampleCount (const juce::var& v)
    {
        const double d = (double) v;
        if (! isFinite (d)) return 0;
        return (int) juce::jlimit (0.0, 2.0e9, d);
    }

    // Tempo: finite, musically plausible. Rejecting 0/NaN/inf here is what
    // keeps Session's own llround(60.0/bpm * ...) out of undefined behaviour.
    bool validBpm (double bpm) { return isFinite (bpm) && bpm > 1.0 && bpm < 1000.0; }
}

static juce::var layerToVar (const LayerSnapshot& l)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("filePath", l.filePath);
    o->setProperty ("assetId", l.assetId);
    o->setProperty ("regionStart", l.regionStart);
    o->setProperty ("regionLength", l.regionLength);
    o->setProperty ("gain", (double) l.gain);
    o->setProperty ("fadeInSamples", l.fadeInSamples);
    o->setProperty ("fadeOutSamples", l.fadeOutSamples);
    o->setProperty ("trimmed", l.trimmed);
    o->setProperty ("enabled", l.enabled);
    o->setProperty ("nameOverride", l.nameOverride);
    o->setProperty ("colourSet", l.colourSet);
    o->setProperty ("colourArgb", (int) l.colourArgb);
    o->setProperty ("linkedPad", l.linkedPad);
    return juce::var (o);
}

static LayerSnapshot layerFromVar (const juce::var& v)
{
    LayerSnapshot l;
    if (auto* o = v.getDynamicObject())
    {
        l.filePath       = o->getProperty ("filePath").toString();
        l.assetId        = o->getProperty ("assetId").toString();   // empty for pre-Milestone-16 files -- fully backward compatible
        // clamped: these are sample counts consumed by the audio thread's
        // own index/envelope math (see this file's SECURITY note)
        l.regionStart    = clampSampleCount (o->getProperty ("regionStart"));
        l.regionLength   = clampSampleCount (o->getProperty ("regionLength"));
        l.gain           = clampGain (o->getProperty ("gain"), l.gain);
        l.fadeInSamples  = clampSampleCount (o->getProperty ("fadeInSamples"));
        l.fadeOutSamples = clampSampleCount (o->getProperty ("fadeOutSamples"));
        l.trimmed        = (bool) o->getProperty ("trimmed");
        l.enabled        = (bool) o->getProperty ("enabled");
        l.nameOverride   = o->getProperty ("nameOverride").toString();
        l.colourSet      = (bool) o->getProperty ("colourSet");
        l.colourArgb     = (juce::uint32) (int) o->getProperty ("colourArgb");
        // hasProperty guard matters here specifically: a plain
        // (int) getProperty(...) on a MISSING property returns 0, a valid
        // pad index (Pad 1), not -1 (no link) -- every project saved before
        // this field existed would otherwise silently gain a false link.
        l.linkedPad = o->hasProperty ("linkedPad") ? (int) o->getProperty ("linkedPad") : -1;
        if (l.linkedPad < 0 || l.linkedPad > 11) l.linkedPad = -1;   // only 12 pads exist
    }
    return l;
}

static juce::var deckToVar (const DeckSnapshot& d)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("flatIndex", d.flatIndex);
    o->setProperty ("tempoOverrideBpm", d.tempoOverrideBpm);
    o->setProperty ("sourceBpm", d.sourceBpm);
    o->setProperty ("stemMode", d.stemMode);
    if (d.clickFile.isNotEmpty()) o->setProperty ("clickFile", d.clickFile);
    if (d.guideFile.isNotEmpty()) o->setProperty ("guideFile", d.guideFile);
    o->setProperty ("useSongClick", d.useSongClick);
    o->setProperty ("useSongGuide", d.useSongGuide);
    if (d.meter.isNotEmpty()) o->setProperty ("meter", d.meter);
    o->setProperty ("rowName", d.rowName);
    o->setProperty ("rowColourSet", d.rowColourSet);
    o->setProperty ("rowColourArgb", (int) d.rowColourArgb);
    o->setProperty ("rowNotes", d.rowNotes);
    o->setProperty ("rowTags", d.rowTags);
    {
        juce::Array<juce::var> sections;
        for (auto& s : d.sections)
        {
            auto* so = new juce::DynamicObject();
            so->setProperty ("name", s.name);
            so->setProperty ("startBar", s.startBar);
            so->setProperty ("colourArgb", (int) s.colourArgb);
            so->setProperty ("skip", s.skip);
            so->setProperty ("optional", s.optional);
            so->setProperty ("loopOnEntry", s.loopOnEntry);
            so->setProperty ("pauseAfter", s.pauseAfter);
            if (s.cue.isNotEmpty()) so->setProperty ("cue", s.cue);
            sections.add (juce::var (so));
        }
        o->setProperty ("sections", sections);
        o->setProperty ("arrangementLengthBars", d.arrangementLengthBars);
        o->setProperty ("endBehaviour", d.endBehaviour);
        o->setProperty ("endFadeSeconds", d.endFadeSeconds);
        o->setProperty ("countInBars", d.countInBars);
        o->setProperty ("guideClick", d.guideClick);
        o->setProperty ("guideCues", d.guideCues);
        o->setProperty ("cueLeadBars", d.cueLeadBars);
        o->setProperty ("cueCounts", d.cueCounts);
    }
    juce::Array<juce::var> layers;
    for (auto& l : d.layers) layers.add (layerToVar (l));
    o->setProperty ("layers", layers);
    return juce::var (o);
}

static DeckSnapshot deckFromVar (const juce::var& v)
{
    DeckSnapshot d;
    if (auto* o = v.getDynamicObject())
    {
        // flatIndex is re-checked against kNumDecks by applySnapshot (that
        // check predates this pass); clamping the obviously-out-of-range
        // values here keeps the invariant local to the parser too.
        d.flatIndex        = juce::jlimit (0, kMaxDecks, (int) o->getProperty ("flatIndex"));
        // -1 = "no override"; any non-finite or implausible bpm falls back
        // to that rather than reaching Session::setTempo (llround UB).
        {
            const double bpm = (double) o->getProperty ("tempoOverrideBpm");
            d.tempoOverrideBpm = validBpm (bpm) ? bpm : -1.0;
            const double src = (double) o->getProperty ("sourceBpm");
            d.sourceBpm = validBpm (src) ? src : -1.0;
        }
        d.stemMode = o->hasProperty ("stemMode") ? (bool) o->getProperty ("stemMode") : true;
        d.clickFile    = o->getProperty ("clickFile").toString().substring (0, 1024);
        d.guideFile    = o->getProperty ("guideFile").toString().substring (0, 1024);
        d.useSongClick = o->hasProperty ("useSongClick") ? (bool) o->getProperty ("useSongClick") : true;
        d.useSongGuide = o->hasProperty ("useSongGuide") ? (bool) o->getProperty ("useSongGuide") : true;
        d.meter        = o->getProperty ("meter").toString().substring (0, 16);   // matched against the app's list; unknown is ignored
        d.rowName          = o->getProperty ("rowName").toString();
        d.rowColourSet     = (bool) o->getProperty ("rowColourSet");
        d.rowColourArgb    = (juce::uint32) (int) o->getProperty ("rowColourArgb");
        d.rowNotes         = o->getProperty ("rowNotes").toString();
        d.rowTags          = o->getProperty ("rowTags").toString();
        // Sections: capped at 256 per song (a 32-bar stem cannot have more
        // than 32 anyway); startBar clamped to a sane range so a crafted file
        // cannot produce a negative or absurd bar that later multiplies into
        // a sample position.
        if (auto* sections = o->getProperty ("sections").getArray())
        {
            for (int i = 0; i < sections->size() && i < 256; ++i)
            {
                if (auto* so = (*sections)[i].getDynamicObject())
                {
                    DeckSnapshot::SectionSnapshot s;
                    s.name        = so->getProperty ("name").toString().substring (0, 64);
                    s.startBar    = juce::jlimit (0, 100000, (int) so->getProperty ("startBar"));
                    s.colourArgb  = (juce::uint32) (int) so->getProperty ("colourArgb");
                    s.skip        = (bool) so->getProperty ("skip");
                    s.optional    = (bool) so->getProperty ("optional");
                    s.loopOnEntry = (bool) so->getProperty ("loopOnEntry");
                    s.pauseAfter  = (bool) so->getProperty ("pauseAfter");
                    // a cue stem names a file inside the app's own bank; it is
                    // only ever looked up in a map, never used as a path
                    s.cue         = so->getProperty ("cue").toString().substring (0, 32);
                    d.sections.push_back (s);
                }
            }
        }
        d.arrangementLengthBars = juce::jlimit (0, 100000, (int) o->getProperty ("arrangementLengthBars"));
        d.endBehaviour          = juce::jlimit (0, 5, o->hasProperty ("endBehaviour") ? (int) o->getProperty ("endBehaviour") : 2);
        d.endFadeSeconds        = o->hasProperty ("endFadeSeconds") ? juce::jlimit (1, 4, (int) o->getProperty ("endFadeSeconds")) : 2;
        d.countInBars           = juce::jlimit (0, 8, (int) o->getProperty ("countInBars"));
        d.guideClick            = (bool) o->getProperty ("guideClick");
        d.guideCues             = (bool) o->getProperty ("guideCues");
        d.cueLeadBars           = juce::jlimit (1, 8, o->hasProperty ("cueLeadBars") ? (int) o->getProperty ("cueLeadBars") : 2);
        d.cueCounts             = o->hasProperty ("cueCounts") ? (bool) o->getProperty ("cueCounts") : true;
        if (auto* layers = o->getProperty ("layers").getArray())
        {
            // PX-B: was a hard 4. Left alone, every project silently lost
            // layers 5-8 on reload the moment the deck grew.
            for (int i = 0; i < layers->size() && i < (int) d.layers.size(); ++i)
                d.layers[(size_t) i] = layerFromVar ((*layers)[i]);
        }
    }
    return d;
}

static juce::var voiceToVar (const VoiceSnapshot& v)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("index", v.index);
    o->setProperty ("filePath", v.filePath);
    o->setProperty ("assetId", v.assetId);
    o->setProperty ("loop", v.loop);
    o->setProperty ("gain", (double) v.gain);
    o->setProperty ("enabled", v.enabled);
    o->setProperty ("soloed", v.soloed);
    o->setProperty ("nameOverride", v.nameOverride);
    o->setProperty ("colourSet", v.colourSet);
    o->setProperty ("colourArgb", (int) v.colourArgb);
    return juce::var (o);
}

static VoiceSnapshot voiceFromVar (const juce::var& v)
{
    VoiceSnapshot s;
    if (auto* o = v.getDynamicObject())
    {
        // index selects a pad/FX slot; applySnapshot re-checks 0..11, but an
        // out-of-range value is rejected here so it can never be used as an
        // index by any future caller either. -1 = "drop this entry".
        s.index    = (int) o->getProperty ("index");
        if (s.index < 0 || s.index > 11) s.index = -1;
        s.filePath = o->getProperty ("filePath").toString();
        s.assetId  = o->getProperty ("assetId").toString();
        s.loop     = (bool) o->getProperty ("loop");
        s.gain     = clampGain (o->getProperty ("gain"), s.gain);
        // hasProperty guard: a missing "enabled" must mean unmuted (true),
        // not (bool) getProperty's own missing-property default of false --
        // same reasoning as LayerSnapshot::linkedPad's own guard above.
        s.enabled      = o->hasProperty ("enabled") ? (bool) o->getProperty ("enabled") : true;
        s.soloed       = (bool) o->getProperty ("soloed");
        s.nameOverride = o->getProperty ("nameOverride").toString();
        s.colourSet    = (bool) o->getProperty ("colourSet");
        s.colourArgb   = (juce::uint32) (int) o->getProperty ("colourArgb");
    }
    return s;
}

static juce::var mixerChannelToVar (const MixerChannelSnapshot& c)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("gain", (double) c.gain);
    o->setProperty ("mute", c.mute);
    o->setProperty ("solo", c.solo);
    o->setProperty ("outputRoute", c.outputRoute);
    return juce::var (o);
}

static MixerChannelSnapshot mixerChannelFromVar (const juce::var& v)
{
    MixerChannelSnapshot c;
    if (auto* o = v.getDynamicObject())
    {
        c.gain = clampGain (o->getProperty ("gain"), c.gain);
        c.mute = (bool) o->getProperty ("mute");
        c.solo = (bool) o->getProperty ("solo");
        // Main, a stereo pair or a mono output. Mixer.h re-clamps against the
        // live device every block; this keeps the stored value honest as well.
        c.outputRoute = MixerChannelSnapshot::sanitiseRoute ((int) o->getProperty ("outputRoute"));
    }
    return c;
}

static juce::var sceneToVar (const SceneSnapshot& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("filled", s.filled);
    o->setProperty ("name", s.name);
    o->setProperty ("signatureIndex", s.signatureIndex);
    o->setProperty ("activeSlot", s.activeSlot);
    o->setProperty ("bpm", s.bpm);
    juce::Array<juce::var> tabEnabled;
    for (bool b : s.tabEnabled) tabEnabled.add (b);
    o->setProperty ("tabEnabled", tabEnabled);
    juce::Array<juce::var> padActive;
    for (bool b : s.padActive) padActive.add (b);
    o->setProperty ("padActive", padActive);
    o->setProperty ("colourSet", s.colourSet);
    o->setProperty ("colourArgb", (int) s.colourArgb);
    return juce::var (o);
}

static SceneSnapshot sceneFromVar (const juce::var& v)
{
    SceneSnapshot s;
    if (auto* o = v.getDynamicObject())
    {
        s.filled         = (bool) o->getProperty ("filled");
        s.name           = o->getProperty ("name").toString();
        // SECURITY (both clamps are load-bearing, not cosmetic):
        //   signatureIndex -> indexes SignatureManager's 7-entry array.
        //   activeSlot     -> added to a signature's firstDeckIndex and used
        //                     to index session.decks[] when the scene is
        //                     recalled. Unclamped, a crafted scene turned a
        //                     scene-button click into an out-of-bounds WRITE.
        s.signatureIndex = juce::jlimit (0, 6, (int) o->getProperty ("signatureIndex"));
        s.activeSlot     = juce::jlimit (0, 7, (int) o->getProperty ("activeSlot"));
        // A 0/missing/NaN bpm reached Session::setTempo, whose
        // llround(60.0/bpm * ...) is undefined behaviour on the AUDIO thread.
        {
            const double bpm = (double) o->getProperty ("bpm");
            if (validBpm (bpm)) s.bpm = bpm;   // else keep the struct default (120)
        }
        // A project written before PX-B carries only 4 entries. The
        // remaining four keep their default (enabled), so an old scene
        // recalls exactly as it always did and the new layers are simply
        // audible rather than mysteriously muted.
        if (auto* tabEnabled = o->getProperty ("tabEnabled").getArray())
            for (int i = 0; i < tabEnabled->size() && i < SceneSnapshot::kSceneTabs; ++i)
                s.tabEnabled[(size_t) i] = (bool) (*tabEnabled)[i];
        if (auto* padActive = o->getProperty ("padActive").getArray())
            for (int i = 0; i < padActive->size() && i < 12; ++i)
                s.padActive[(size_t) i] = (bool) (*padActive)[i];
        s.colourSet  = (bool) o->getProperty ("colourSet");
        s.colourArgb = (juce::uint32) (int) o->getProperty ("colourArgb");
    }
    return s;
}

static juce::var settingsToVar (const SettingsSnapshot& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("metronomeEnabled", s.metronomeEnabled);
    o->setProperty ("onePadAtATime", s.onePadAtATime);
    o->setProperty ("meterVisible", s.meterVisible);
    o->setProperty ("tempoLockEnabled", s.tempoLockEnabled);
    juce::Array<juce::var> tIn, tSt, tId, tState, tMidi;
    for (int i = 0; i < SettingsSnapshot::kLiveTracks; ++i)
    {
        tIn.add (s.trackInput[(size_t) i]);
        tSt.add (s.trackStereo[(size_t) i]);
        tId.add (s.trackInstrumentId[(size_t) i]);
        tState.add (s.trackInstrumentState[(size_t) i]);
        tMidi.add (s.trackMidiChannel[(size_t) i]);
    }
    o->setProperty ("trackInput", tIn);
    o->setProperty ("trackStereo", tSt);
    o->setProperty ("trackInstrumentId", tId);
    o->setProperty ("trackInstrumentState", tState);
    o->setProperty ("trackMidiChannel", tMidi);
    juce::Array<juce::var> stripOnArr, stripStateArr;
    for (int i = 0; i < SettingsSnapshot::kStrips; ++i) { stripOnArr.add (s.stripOn[(size_t) i]); stripStateArr.add (s.stripState[(size_t) i]); }
    o->setProperty ("stripOn", stripOnArr);
    o->setProperty ("stripState", stripStateArr);
    o->setProperty ("webGain", (double) s.webGain);
    o->setProperty ("webMute", s.webMute);
    o->setProperty ("webRoute", s.webRoute);
    return juce::var (o);
}

static SettingsSnapshot settingsFromVar (const juce::var& v)
{
    SettingsSnapshot s;
    if (auto* o = v.getDynamicObject())
    {
        s.metronomeEnabled = (bool) o->getProperty ("metronomeEnabled");
        s.onePadAtATime    = (bool) o->getProperty ("onePadAtATime");
        s.meterVisible     = (bool) o->getProperty ("meterVisible");
        s.tempoLockEnabled = (bool) o->getProperty ("tempoLockEnabled");
        if (auto* ch = o->getProperty ("liveInputChannel").getArray())
            for (int i = 0; i < ch->size() && i < SettingsSnapshot::kLiveColumns; ++i)
                s.liveInputChannel[(size_t) i] = juce::jlimit (-1, 63, (int) (*ch)[i]);   // a device has at most 64 inputs we care about
        if (auto* st = o->getProperty ("liveInputStereo").getArray())
            for (int i = 0; i < st->size() && i < SettingsSnapshot::kLiveColumns; ++i)
                s.liveInputStereo[(size_t) i] = (bool) (*st)[i];
        // an identifier is only ever matched against the scanned plugin list,
        // never opened as a path; the state blob is handed to the plugin
        // whose identifier matched, capped in InstrumentSlot
        if (auto* ids = o->getProperty ("instrumentId").getArray())
            for (int i = 0; i < ids->size() && i < SettingsSnapshot::kLiveColumns; ++i)
                s.instrumentId[(size_t) i] = (*ids)[i].toString().substring (0, 512);
        if (auto* states = o->getProperty ("instrumentState").getArray())
            for (int i = 0; i < states->size() && i < SettingsSnapshot::kLiveColumns; ++i)
                s.instrumentState[(size_t) i] = (*states)[i].toString();
        if (auto* on = o->getProperty ("stripOn").getArray())
            for (int i = 0; i < on->size() && i < SettingsSnapshot::kStrips; ++i)
                s.stripOn[(size_t) i] = (bool) (*on)[i];
        // handed only to our own compiled-in strip, which parses it as XML
        if (auto* st = o->getProperty ("stripState").getArray())
            for (int i = 0; i < st->size() && i < SettingsSnapshot::kStrips; ++i)
                s.stripState[(size_t) i] = (*st)[i].toString().substring (0, 1 << 20);

        if (o->hasProperty ("trackInput"))
        {
            if (auto* a = o->getProperty ("trackInput").getArray())
                for (int i = 0; i < a->size() && i < SettingsSnapshot::kLiveTracks; ++i)
                    s.trackInput[(size_t) i] = juce::jlimit (-1, 63, (int) (*a)[i]);
            if (auto* a = o->getProperty ("trackStereo").getArray())
                for (int i = 0; i < a->size() && i < SettingsSnapshot::kLiveTracks; ++i)
                    s.trackStereo[(size_t) i] = (bool) (*a)[i];
            // matched against the scanned plugin list only, never opened as a path
            if (auto* a = o->getProperty ("trackInstrumentId").getArray())
                for (int i = 0; i < a->size() && i < SettingsSnapshot::kLiveTracks; ++i)
                    s.trackInstrumentId[(size_t) i] = (*a)[i].toString().substring (0, 512);
            if (auto* a = o->getProperty ("trackInstrumentState").getArray())
                for (int i = 0; i < a->size() && i < SettingsSnapshot::kLiveTracks; ++i)
                    s.trackInstrumentState[(size_t) i] = (*a)[i].toString();
            if (auto* a = o->getProperty ("trackMidiChannel").getArray())
                for (int i = 0; i < a->size() && i < SettingsSnapshot::kLiveTracks; ++i)
                    s.trackMidiChannel[(size_t) i] = juce::jlimit (0, 16, (int) (*a)[i]);
        }
        else
        {
            // Owner: "the eight decks should be available for stems." A file
            // from before the live tracks had its mics and instruments ON deck
            // columns; each moves, in column order, onto the next free live
            // track, and takes that column's channel strip with it. The
            // column is left as a plain stem column.
            int t = 0;
            for (int col = 0; col < SettingsSnapshot::kLiveColumns && t < SettingsSnapshot::kLiveTracks; ++col)
            {
                const bool hasInst  = s.instrumentId[(size_t) col].isNotEmpty();
                const bool hasInput = s.liveInputChannel[(size_t) col] >= 0;
                if (! hasInst && ! hasInput) continue;
                if (hasInst)
                {
                    s.trackInstrumentId[(size_t) t]    = s.instrumentId[(size_t) col];
                    s.trackInstrumentState[(size_t) t] = s.instrumentState[(size_t) col];
                }
                else
                {
                    s.trackInput[(size_t) t]  = s.liveInputChannel[(size_t) col];
                    s.trackStereo[(size_t) t] = s.liveInputStereo[(size_t) col];
                }
                const size_t trackStrip = (size_t) (SettingsSnapshot::kLiveColumns + t);
                s.stripOn[trackStrip]    = s.stripOn[(size_t) col];
                s.stripState[trackStrip] = s.stripState[(size_t) col];
                s.stripOn[(size_t) col]  = false;
                s.stripState[(size_t) col] = {};
                ++t;
            }
        }
        // the legacy per-column fields have done their job
        s.liveInputChannel.fill (-1);
        s.liveInputStereo.fill (false);
        for (auto& x : s.instrumentId) x = {};
        for (auto& x : s.instrumentState) x = {};
        if (o->hasProperty ("webGain")) s.webGain = clampGain (o->getProperty ("webGain"), 1.0f);
        s.webMute = (bool) o->getProperty ("webMute");
        s.webRoute = MixerChannelSnapshot::sanitiseRoute ((int) o->getProperty ("webRoute"));
    }
    return s;
}

juce::var toVar (const ProjectSnapshot& snapshot)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", snapshot.version);
    root->setProperty ("settings", settingsToVar (snapshot.settings));
    root->setProperty ("masterTempoBpm", snapshot.masterTempoBpm);
    root->setProperty ("viewedSignature", snapshot.viewedSignature);
    root->setProperty ("masterGain", (double) snapshot.masterGain);

    juce::Array<juce::var> mixerChannels;
    for (auto& c : snapshot.mixerChannels) mixerChannels.add (mixerChannelToVar (c));
    root->setProperty ("mixerChannels", mixerChannels);

    juce::Array<juce::var> decks;
    for (auto& d : snapshot.decks) decks.add (deckToVar (d));
    root->setProperty ("decks", decks);

    juce::Array<juce::var> pads;
    for (auto& p : snapshot.pads) pads.add (voiceToVar (p));
    root->setProperty ("pads", pads);

    juce::Array<juce::var> fx;
    for (auto& f : snapshot.fx) fx.add (voiceToVar (f));
    root->setProperty ("fx", fx);

    juce::Array<juce::var> scenes;
    for (auto& s : snapshot.scenes) scenes.add (sceneToVar (s));
    root->setProperty ("scenes", scenes);

    {
        juce::Array<juce::var> setlist;
        for (int idx : snapshot.setlist) setlist.add (idx);
        root->setProperty ("setlist", setlist);
        root->setProperty ("jumpMode", snapshot.jumpMode);
    }

    juce::Array<juce::var> sigColourSet;
    for (bool b : snapshot.signatureColourSet) sigColourSet.add (b);
    root->setProperty ("signatureColourSet", sigColourSet);

    juce::Array<juce::var> sigColourArgb;
    for (juce::uint32 c : snapshot.signatureColourArgb) sigColourArgb.add ((int) c);
    root->setProperty ("signatureColourArgb", sigColourArgb);

    return juce::var (root);
}

bool fromVar (const juce::var& v, ProjectSnapshot& out)
{
    auto* root = v.getDynamicObject();
    if (root == nullptr) return false;

    const int version = (int) root->getProperty ("version");
    if (version != kCurrentVersion) return false;   // no migration exists yet -- caller falls back to defaults

    ProjectSnapshot snapshot;
    snapshot.version         = version;
    snapshot.settings        = settingsFromVar (root->getProperty ("settings"));
    // Robustness (caught live via a partial project file): a MISSING numeric
    // property reads as 0 through getProperty, which silently zeroed the
    // master tempo and volume -- an unplayable session from one damaged or
    // hand-truncated file. Missing/invalid values fall back to the struct
    // defaults instead (same hasProperty-guard reasoning as linkedPad's).
    if (root->hasProperty ("masterTempoBpm"))
    {
        const double bpm = (double) root->getProperty ("masterTempoBpm");
        if (validBpm (bpm)) snapshot.masterTempoBpm = bpm;   // finite + in range: see validBpm
    }
    // SECURITY: viewedSignature indexes SignatureManager's 7-entry array and
    // is consumed immediately by refreshSlotLabels()/paint(). Unclamped, a
    // one-line edit to a .perform file produced out-of-bounds reads AND a
    // juce::String refcount increment through a wild pointer, on open, with
    // no user interaction beyond opening the file.
    snapshot.viewedSignature = juce::jlimit (0, 6, (int) root->getProperty ("viewedSignature"));
    if (root->hasProperty ("masterGain"))
        snapshot.masterGain = clampGain (root->getProperty ("masterGain"), snapshot.masterGain);

    if (auto* mixerChannels = root->getProperty ("mixerChannels").getArray())
    {
        // A project saved before PX-B has 7 channels: Tab1-4, Pads, Fx, Metro.
        // Tab5-8 and then Live1-4 were inserted before Pads, so those move
        // from 4/5/6 to 12/13/14. 11- and 12-entry files (Tab1-8, Pads, Fx,
        // Metro, Cues) move everything from index 8 up by the four live tracks.
        const int count = mixerChannels->size();
        for (int i = 0; i < count; ++i)
        {
            int dest = i;
            if (count == 7 && i >= 4)                  dest = i + 8;
            else if ((count == 11 || count == 12) && i >= 8) dest = i + 4;
            if (dest >= ProjectSnapshot::kMixerChannels) break;
            snapshot.mixerChannels[(size_t) dest] = mixerChannelFromVar ((*mixerChannels)[i]);
        }
    }

    // Before the native click existed, Settings > Metronome worked by MUTING
    // the Metro strip, and that mute was saved with the project. Such a file
    // (no deck carries the guide fields yet) would load with the Click strip
    // silently muted, so the mute is dropped: the strip's own M button is
    // the only mute now, and the metronome setting gates the loop click.
    {
        bool anyGuideField = false;
        if (auto* decks = root->getProperty ("decks").getArray())
            for (const auto& dv : *decks)
                if (auto* o = dv.getDynamicObject(); o != nullptr && o->hasProperty ("guideClick")) anyGuideField = true;
        if (! anyGuideField)
            snapshot.mixerChannels[14].mute = false;   // Metro, after the remap above
    }

    if (auto* decks = root->getProperty ("decks").getArray())
        for (auto& d : *decks) snapshot.decks.push_back (deckFromVar (d));

    if (auto* pads = root->getProperty ("pads").getArray())
        for (auto& p : *pads) snapshot.pads.push_back (voiceFromVar (p));

    if (auto* fx = root->getProperty ("fx").getArray())
        for (auto& f : *fx) snapshot.fx.push_back (voiceFromVar (f));

    if (auto* scenes = root->getProperty ("scenes").getArray())
        for (int i = 0; i < scenes->size() && i < 8; ++i)
            snapshot.scenes[(size_t) i] = sceneFromVar ((*scenes)[i]);

    // SECURITY: setlist entries become deck indices. Clamp to the deck array
    // and cap the count -- applySnapshot re-checks, but the parser is the
    // boundary and should not hand out-of-range indices past it.
    if (auto* setlist = root->getProperty ("setlist").getArray())
        for (int i = 0; i < setlist->size() && i < 256; ++i)
        {
            const int idx = (int) (*setlist)[i];
            if (idx >= 0 && idx <= kMaxDecks) snapshot.setlist.push_back (idx);
        }
    snapshot.jumpMode = juce::jlimit (0, 2, root->hasProperty ("jumpMode") ? (int) root->getProperty ("jumpMode") : 1);

    if (auto* sigColourSet = root->getProperty ("signatureColourSet").getArray())
        for (int i = 0; i < sigColourSet->size() && i < 7; ++i)
            snapshot.signatureColourSet[(size_t) i] = (bool) (*sigColourSet)[i];

    if (auto* sigColourArgb = root->getProperty ("signatureColourArgb").getArray())
        for (int i = 0; i < sigColourArgb->size() && i < 7; ++i)
            snapshot.signatureColourArgb[(size_t) i] = (juce::uint32) (int) (*sigColourArgb)[i];

    out = snapshot;
    return true;
}

bool saveToFile (const ProjectSnapshot& snapshot, const juce::File& file)
{
    const juce::var v = toVar (snapshot);
    const juce::String json = juce::JSON::toString (v);
    return file.replaceWithText (json);
}

bool loadFromFile (const juce::File& file, ProjectSnapshot& out)
{
    if (! file.existsAsFile()) return false;
    const juce::String text = file.loadFileAsString();
    if (text.isEmpty()) return false;

    juce::var parsed;
    if (juce::JSON::parse (text, parsed).failed()) return false;

    return fromVar (parsed, out);
}

} // namespace ezproject
