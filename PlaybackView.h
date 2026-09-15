// ============================================================================
//  PlaybackView.h -- the performance screen.
//
//  What a band actually looks at during a song, borrowed from AbleSet and
//  the worship players (Prime, Playback) and stripped to what matters from
//  a music stand:
//
//    * the CURRENT section, big, in its colour -- and what's NEXT
//    * where we are: bar | beat, tempo, time signature, time left in the song
//    * a horizontal timeline: the song's sections as coloured blocks over the
//      four stems' waveforms, with the playhead sweeping across
//    * the setlist, so "what's after this" is never a question
//    * a transport the MD can drive with one finger: previous/next section,
//      previous/next song, loop this section, jump now, cancel
//
//  The view owns NO playback state. Everything it shows it reads from a
//  PlaybackHost every frame, and everything it changes it asks the host to
//  do. That keeps the engine rules (jumps fire on boundaries, loops are
//  sample-exact) in one place -- SessionComponent -- and makes this file a
//  renderer that cannot get the music wrong.
//
//  Editing sections happens here too: "+ Section" adds one at the playhead,
//  a double-click adds one anywhere on the timeline, and both offer the usual
//  names in one click (the colour follows the name). Drag a boundary to move
//  it; right-click a section for its flags. "Auto-section" drafts a typical
//  worship structure to be corrected rather than built from nothing. Each
//  track's header names it, and solos or mutes it.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "Arrangement.h"
#include "Guide.h"       // ezguide::cueIsOff -- the section menu's Cue choices
#include "CueDetect.h"   // ezcue::DraftSection -- sections read from a cue track
#include "Deck.h"        // ezdeck::kNumLayers -- the lane count follows the engine
#include "UiArt.h"
#include "TouchSupport.h"
#include "SectionNameField.h"   // type "v" -> Verse when naming a section

#include <functional>
#include <vector>

namespace ezplayback
{

//==============================================================================
//  What the view needs from whoever owns the engine.
//==============================================================================
class PlaybackHost
{
public:
    virtual ~PlaybackHost() = default;

    // ---- setlist ----
    virtual int          numSongs() const = 0;
    virtual int          songDeck (int songIndex) const = 0;         // flat deck index
    virtual juce::String songName (int songIndex) const = 0;
    virtual juce::Colour songColour (int songIndex) const = 0;
    virtual bool         songHasAudio (int songIndex) const = 0;
    virtual int          songLengthBars (int songIndex) const = 0;
    virtual double       songLengthSeconds (int songIndex) const = 0;
    virtual int          currentSongIndex() const = 0;               // -1 none
    virtual void         selectSong (int songIndex) = 0;             // cue it (does not start playing)
    virtual void         moveSong (int from, int to) = 0;
    virtual void         removeSong (int songIndex) = 0;
    virtual void         promptAddSong() = 0;                        // host shows its own picker
    virtual void         renameSong (int songIndex) = 0;

    // ---- arrangement of the current song ----
    virtual ezarr::Arrangement* currentArrangement() = 0;            // nullptr when no song
    virtual void arrangementEdited() = 0;                            // persist + re-arm

    // ---- playback state ----
    virtual bool   isPlaying() const = 0;
    virtual bool   isCountingIn() const = 0;
    virtual double currentBar() const = 0;                           // fractional, 0-based
    virtual int    currentSection() const = 0;                       // -1
    virtual int    queuedSection() const = 0;                        // -1
    virtual bool   isLoopingSection() const = 0;
    virtual double tempoBpm() const = 0;
    virtual int    beatsPerBar() const = 0;
    virtual double secondsPerBar() const = 0;
    virtual int    jumpMode() const = 0;                             // 0 next bar, 1 end of section, 2 now
    virtual void   setJumpMode (int) = 0;
    virtual int    countInBars() const = 0;
    virtual void   setCountInBars (int) = 0;

    // ---- waveform data for the lanes ----
    virtual const std::vector<float>* layerSamples (int songIndex, int layer) const = 0;
    virtual juce::String layerName (int songIndex, int layer) const = 0;
    virtual bool layerEnabled (int songIndex, int layer) const = 0;
    virtual void toggleLayer (int songIndex, int layer) = 0;
    /** Names a track ("DRUMS", "BGVs"); an empty name goes back to the file name. */
    virtual void setLayerName (int songIndex, int layer, const juce::String& name) { juce::ignoreUnused (songIndex, layer, name); }
    virtual bool layerHasCustomName (int songIndex, int layer) const { juce::ignoreUnused (songIndex, layer); return false; }
    /** The audio file behind a track, shown small inside its lane. */
    virtual juce::String layerFileName (int songIndex, int layer) const { juce::ignoreUnused (songIndex, layer); return {}; }
    /** True when a layer makes sound without holding an audio file --
        a live input or a hosted instrument. Defaulted to false so the
        view can already ask the question before PX-C/PX-D exist to
        answer it; a host that never sets a layer mode simply keeps the
        old behaviour. */
    virtual bool layerIsLive (int songIndex, int layer) const
    {
        juce::ignoreUnused (songIndex, layer);
        return false;
    }

    // ---- commands ----
    virtual void playStop() = 0;
    virtual void playWithCountIn() = 0;
    virtual void jumpToSection (int sectionIndex) = 0;
    virtual void jumpNow() = 0;
    virtual void cancelJump() = 0;
    virtual void toggleLoopSection() = 0;
    virtual void nextSection() = 0;
    virtual void prevSection() = 0;
    virtual void nextSong() = 0;
    virtual void prevSong() = 0;
    virtual void seekToBar (double bar) = 0;

    // ---- native click and cues (Guide.h) ----
    virtual bool guideClick() const { return false; }
    virtual void setGuideClick (bool) {}
    virtual bool guideCues() const { return false; }
    virtual void setGuideCues (bool) {}
    virtual int  cueLeadBars() const { return 2; }
    virtual void setCueLeadBars (int) {}
    virtual bool cueCounts() const { return true; }
    virtual void setCueCounts (bool) {}
    /** Every spoken cue the bank holds, grouped: "Song Form/Chorus-2". */
    virtual juce::StringArray cueNames() const { return {}; }
    virtual bool cueBankLoaded() const { return false; }
    virtual void previewCue (const juce::String& stem) { juce::ignoreUnused (stem); }

    // ---- sections from the song's own cue track (CueDetect.h) ----
    /** Listens to one track of the song and returns the sections its cues
        imply. Empty when the track has no audio or no cues were heard. */
    /** layer -1 = the song's own guide track. Listens in the background, then
        opens a review window where each suggested section can be heard,
        renamed, moved or dropped before it replaces the song's sections. */
    virtual void listenForSections (int songIndex, int layer) { juce::ignoreUnused (songIndex, layer); }
    /** The song's own guide track, loaded beside the row rather than on a deck. */
    virtual bool songHasGuideTrack (int songIndex) const { juce::ignoreUnused (songIndex); return false; }
};

//==============================================================================
namespace tokens
{
    const juce::Colour shell     (0xff0c0c17u);
    const juce::Colour workspace (0xff07070fu);
    const juce::Colour card      (0xff151527u);
    const juce::Colour border    (0xff2b2b4du);
    const juce::Colour indigo    (0xff7c5cffu);
    const juce::Colour bright    (0xfff2f0ffu);
    const juce::Colour dim       (0xffa3a6ccu);
    const juce::Colour faint     (0xff6f7099u);
    const juce::Colour play      (0xff2ee86au);
    const juce::Colour queued    (0xffffc933u);
    const juce::Colour danger    (0xffff3b5cu);
    // ring hues -- the same cyan/violet/magenta the deck lanes use
    const juce::Colour ringA     (0xff00d9ffu);
    const juce::Colour ringB     (0xffa855f7u);
    const juce::Colour ringC     (0xffff2d95u);
}

inline juce::Colour sectionColour (const ezarr::Arrangement& a, int i)
{
    if (i < 0 || i >= (int) a.sections.size()) return tokens::faint;
    const auto c = a.sections[(size_t) i].colourArgb;
    return juce::Colour (c != 0 ? c : ezarr::defaultSectionColour (i));
}

inline juce::String formatClock (double seconds)
{
    const int s = (int) std::floor ((std::max) (0.0, seconds) + 0.5);
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}

//==============================================================================
//  Stem identity: a lane is recognised by its icon and colour before its name.
//  The icon follows the track NAME ("DRUMS", "Kick", "Bass Gtr", "BGVs"...),
//  so naming a track is the only thing a performer has to do -- nothing new
//  is stored, and a stem still named after its file gets a sensible icon.
//==============================================================================
enum class StemKind { drums, bass, keys, synth, guitar, vocals, pads, strings, brass, click, other };

inline StemKind stemKindFor (const juce::String& rawName)
{
    const auto n = rawName.toLowerCase();
    auto has = [&n] (const char* w) { return n.contains (w); };
    if (has ("click") || has ("cue") || has ("guide") || has ("metro") || has ("count"))                  return StemKind::click;
    if (has ("bass") || has ("808"))                                                                       return StemKind::bass;
    if (has ("drum") || has ("perc") || has ("kick") || has ("snare") || has ("hihat") || has ("hi-hat")
        || has ("shaker") || has ("tamb") || has ("cymbal") || has ("toms") || has ("clap") || has ("cong")
        || has ("bongo") || has ("djembe") || has ("cajon") || has ("cowbell") || has ("agogo"))          return StemKind::drums;
    if (has ("clav"))                                                                                      return StemKind::keys;
    if (has ("gtr") || has ("guitar"))                                                                     return StemKind::guitar;
    if (has ("vox") || has ("vocal") || has ("choir") || has ("bgv") || has ("sing") || has ("harmon"))     return StemKind::vocals;
    if (has ("key") || has ("piano") || has ("rhodes") || has ("organ") || has ("wurli"))                  return StemKind::keys;
    if (has ("synth") || has ("arp") || has ("lead"))                                                      return StemKind::synth;
    if (has ("pad") || has ("atmos") || has ("ambien") || has ("drone") || has ("swell"))                  return StemKind::pads;
    if (has ("string") || has ("violin") || has ("cello") || has ("orch"))                                 return StemKind::strings;
    if (has ("brass") || has ("horn") || has ("trumpet") || has ("sax"))                                   return StemKind::brass;
    return StemKind::other;
}

/** Line-art instrument glyphs, drawn in a unit square and scaled into `r`.
    Drawn rather than bitmaps so they take any lane colour and stay sharp. */
inline void drawStemIcon (juce::Graphics& g, StemKind kind, juce::Rectangle<float> r, juce::Colour c)
{
    const float s = (std::min) (r.getWidth(), r.getHeight());
    if (s < 6.0f) return;
    const auto box = r.withSizeKeepingCentre (s, s);
    const auto xf = juce::AffineTransform::scale (s).translated (box.getX(), box.getY());
    const juce::PathStrokeType stroke ((std::max) (1.4f, s * 0.075f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    const float halfPi = juce::MathConstants<float>::halfPi;
    const float twoPi  = juce::MathConstants<float>::twoPi;
    juce::Path line, fill;

    switch (kind)
    {
        case StemKind::drums:
            line.addEllipse (0.14f, 0.34f, 0.72f, 0.22f);
            line.startNewSubPath (0.14f, 0.45f); line.lineTo (0.14f, 0.74f);
            line.startNewSubPath (0.86f, 0.45f); line.lineTo (0.86f, 0.74f);
            line.addCentredArc (0.5f, 0.74f, 0.36f, 0.11f, 0.0f, halfPi, halfPi * 3.0f, true);
            line.startNewSubPath (0.22f, 0.08f); line.lineTo (0.44f, 0.36f);
            line.startNewSubPath (0.78f, 0.08f); line.lineTo (0.56f, 0.36f);
            break;

        case StemKind::bass:
        case StemKind::guitar:
        {
            const bool bass = kind == StemKind::bass;
            fill.addEllipse (0.08f, 0.52f, 0.38f, 0.38f);
            fill.addEllipse (0.18f, 0.40f, 0.26f, 0.26f);
            line.startNewSubPath (0.34f, 0.64f); line.lineTo (bass ? 0.88f : 0.80f, bass ? 0.10f : 0.18f);
            fill.addEllipse (bass ? 0.83f : 0.75f, bass ? 0.04f : 0.12f, 0.12f, 0.12f);
            break;
        }

        case StemKind::keys:
            line.addRoundedRectangle (0.08f, 0.26f, 0.84f, 0.50f, 0.06f);
            for (float x : { 0.29f, 0.50f, 0.71f }) { line.startNewSubPath (x, 0.52f); line.lineTo (x, 0.76f); }
            for (float x : { 0.29f, 0.50f, 0.71f }) fill.addRectangle (x - 0.05f, 0.26f, 0.10f, 0.26f);
            break;

        case StemKind::synth:
            line.addRoundedRectangle (0.08f, 0.44f, 0.84f, 0.40f, 0.06f);
            for (float x : { 0.29f, 0.50f, 0.71f }) fill.addRectangle (x - 0.045f, 0.44f, 0.09f, 0.20f);
            line.startNewSubPath (0.12f, 0.24f);
            for (int i = 1; i <= 16; ++i)
            {
                const float t = (float) i / 16.0f;
                line.lineTo (0.12f + 0.76f * t, 0.24f - 0.10f * std::sin (t * twoPi));
            }
            break;

        case StemKind::vocals:
            fill.addRoundedRectangle (0.36f, 0.06f, 0.28f, 0.46f, 0.14f);
            line.addCentredArc (0.5f, 0.40f, 0.25f, 0.28f, 0.0f, halfPi, halfPi * 3.0f, true);
            line.startNewSubPath (0.5f, 0.68f); line.lineTo (0.5f, 0.88f);
            line.startNewSubPath (0.32f, 0.90f); line.lineTo (0.68f, 0.90f);
            break;

        case StemKind::click:
            line.startNewSubPath (0.26f, 0.90f); line.lineTo (0.74f, 0.90f);
            line.lineTo (0.60f, 0.10f); line.lineTo (0.40f, 0.10f); line.closeSubPath();
            line.startNewSubPath (0.50f, 0.72f); line.lineTo (0.70f, 0.26f);
            fill.addEllipse (0.62f, 0.34f, 0.12f, 0.12f);
            break;

        case StemKind::strings:
            fill.addEllipse (0.26f, 0.36f, 0.48f, 0.52f);
            line.startNewSubPath (0.5f, 0.06f); line.lineTo (0.5f, 0.94f);
            line.startNewSubPath (0.14f, 0.30f); line.lineTo (0.86f, 0.62f);
            break;

        case StemKind::brass:
            line.startNewSubPath (0.10f, 0.50f); line.lineTo (0.62f, 0.50f);
            for (float x : { 0.24f, 0.36f, 0.48f }) { line.startNewSubPath (x, 0.36f); line.lineTo (x, 0.50f); }
            fill.startNewSubPath (0.60f, 0.44f); fill.lineTo (0.92f, 0.22f);
            fill.lineTo (0.92f, 0.78f); fill.lineTo (0.60f, 0.56f); fill.closeSubPath();
            break;

        case StemKind::pads:
            for (int k = 0; k < 2; ++k)
            {
                const float y0 = 0.38f + 0.26f * (float) k;
                line.startNewSubPath (0.08f, y0);
                for (int i = 1; i <= 20; ++i)
                {
                    const float t = (float) i / 20.0f;
                    line.lineTo (0.08f + 0.84f * t, y0 - 0.12f * std::sin (t * twoPi));
                }
            }
            break;

        case StemKind::other:
        default:
        {
            const float hs[] = { 0.30f, 0.62f, 0.86f, 0.52f, 0.72f, 0.36f };
            for (int i = 0; i < 6; ++i)
            {
                const float x = 0.14f + 0.144f * (float) i;
                line.startNewSubPath (x, 0.5f - hs[i] * 0.42f);
                line.lineTo (x, 0.5f + hs[i] * 0.42f);
            }
            break;
        }
    }

    g.setColour (c);
    if (! fill.isEmpty()) g.fillPath (fill, xf);
    if (! line.isEmpty()) g.strokePath (line, stroke, xf);
}

/** A section's colour follows its name, so every Chorus in a set is the same
    colour and a performer reads the structure before the words. Names that
    mean nothing in particular fall back to the positional wheel. */
inline juce::uint32 colourForSectionName (const std::string& raw, int fallbackIndex)
{
    const auto n = juce::String (juce::CharPointer_UTF8 (raw.c_str())).toLowerCase();
    if (n.contains ("pre"))                                                   return 0xff4d7cff;
    if (n.contains ("post"))                                                  return 0xffff6ec7;
    if (n.contains ("chorus"))                                                return 0xffff2d95;
    if (n.contains ("verse"))                                                 return 0xffa855f7;
    if (n.contains ("intro"))                                                 return 0xff00d9ff;
    if (n.contains ("bridge"))                                                return 0xffffa62b;
    if (n.contains ("instr") || n.contains ("interlude") || n.contains ("break") || n.contains ("solo"))
                                                                              return 0xff3dffc0;
    if (n.contains ("tag") || n.contains ("vamp") || n.contains ("refrain"))  return 0xffb6ff2e;
    if (n.contains ("outro") || n.contains ("ending"))                        return 0xffff5c3b;
    return ezarr::defaultSectionColour (fallbackIndex);
}

//==============================================================================
//  The timeline: ruler + section blocks + a header and waveform per track +
//  playhead.
//
//  Lane header (left): icon, NAME, STEM/MUTED/SOLO, and S, M and the pencil.
//  The pencil (or a click on the name) names the track from a preset list or
//  by typing; the icon follows the name.
//
//  Waveforms are rendered once per lane into an image -- a mirrored peak
//  body, a brighter RMS core and a soft glow -- and blitted every frame: the
//  part already played at full strength, what is still to come dimmer, a
//  muted lane faint. That is what makes them read as music rather than a
//  barcode, and it is cheaper than before: two image draws per lane a frame
//  instead of a thousand vertical lines.
//
//  Adding sections: "+ Section" (at the playhead), a double-click anywhere on
//  the ruler or the waveforms, or right-click > Add section. Every route ends
//  in the same one-click name list, and the colour follows the name.
//==============================================================================
class Timeline : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Timeline (PlaybackHost& h) : host (h) { setWantsKeyboardFocus (false); }

    std::function<void (int sectionIndex, juce::Point<int> screenPos)> onSectionMenu;
    std::function<void (int bar, juce::Point<int> screenPos)> onAddSectionAt;
    std::function<void (int layer, juce::Point<int> screenPos)> onLaneMenu;

    /** Same eight hues as the deck columns, so a lane and its column are
        recognisably the same track. */
    static juce::Colour laneColour (int layer)
    {
        static const juce::Colour cols[ezdeck::kNumLayers] = {
            juce::Colour (0xff00d9ffu), juce::Colour (0xffa855f7u),
            juce::Colour (0xffff2d95u), juce::Colour (0xffffa62bu),
            juce::Colour (0xff3dffc0u), juce::Colour (0xff4d7cffu),
            juce::Colour (0xffb6ff2eu), juce::Colour (0xffff5c3bu) };
        return cols[(size_t) juce::jlimit (0, ezdeck::kNumLayers - 1, layer)];
    }

    void rebuildPeaks()
    {
        const int song = host.currentSongIndex();
        if (song != soloSong) { soloLane = -1; soloSong = -1; }

        for (int l = 0; l < ezdeck::kNumLayers; ++l)
        {
            auto& pk = peaks[(size_t) l];
            auto& rm = rms[(size_t) l];
            pk.assign (kPeakRes, 0.0f);
            rm.assign (kPeakRes, 0.0f);
            laneImages[(size_t) l] = juce::Image();

            const auto* samples = song >= 0 ? host.layerSamples (song, l) : nullptr;
            hasAudio[(size_t) l] = samples != nullptr && ! samples->empty();
            if (! hasAudio[(size_t) l]) continue;

            const size_t n = samples->size();
            float loudest = 0.0f;
            for (int b = 0; b < kPeakRes; ++b)
            {
                const size_t s0 = (size_t) ((double) b / kPeakRes * (double) n);
                const size_t s1 = (std::max) (s0 + 1, (size_t) ((double) (b + 1) / kPeakRes * (double) n));
                float mx = 0.0f;
                double sq = 0.0;
                size_t count = 0;
                for (size_t i = s0; i < s1 && i < n; ++i)
                {
                    const float v = std::fabs ((*samples)[i]);
                    mx = (std::max) (mx, v);
                    sq += (double) v * (double) v;
                    ++count;
                }
                pk[(size_t) b] = mx;
                rm[(size_t) b] = count > 0 ? (float) std::sqrt (sq / (double) count) : 0.0f;
                loudest = (std::max) (loudest, mx);
            }

            // A quiet stem (a pad at -18 dB) is scaled up to use its lane: the
            // lane shows the SHAPE of the part; the mixer shows its level.
            if (loudest > 0.0001f)
            {
                const float gain = 0.95f / loudest;
                for (int b = 0; b < kPeakRes; ++b)
                {
                    pk[(size_t) b] *= gain;
                    rm[(size_t) b] = (std::min) (pk[(size_t) b], rm[(size_t) b] * gain * 1.35f);
                }
            }
        }

        // A layer earns a lane if it has audio, or if it is configured as
        // something that makes sound without a file -- a live input or a
        // hosted instrument. Layers 1-4 always keep a lane so the view
        // does not reshape itself under an MD mid-song.
        visibleLanes.clear();
        for (int l = 0; l < ezdeck::kNumLayers; ++l)
            if (l < 4 || hasAudio[(size_t) l] || host.layerIsLive (song, l))
                visibleLanes.push_back (l);

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (tokens::workspace);
        g.fillRoundedRectangle (r.toFloat(), 10.0f);

        auto* arr = host.currentArrangement();
        const int song = host.currentSongIndex();
        const auto geo = geometry();
        if (arr == nullptr || geo.bars <= 0)
        {
            g.setColour (tokens::faint);
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText ("Add a song to the setlist to see its timeline", r, juce::Justification::centred);
            return;
        }

        const auto xForBar = [&geo] (double b) { return (float) geo.ruler.getX() + (float) b * geo.pxPerBar; };

        // ---- corner above the lane headers ----
        g.setColour (tokens::faint);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)).withExtraKerningFactor (0.08f));
        g.drawText ("SECTIONS", juce::Rectangle<int> (geo.inner.getX() + 8, geo.ruler.getY(), kHeaderW - 16, geo.ruler.getHeight()),
                    juce::Justification::centredLeft);

        // ---- section blocks on the ruler ----
        const int current = host.currentSection();
        const int queued  = host.queuedSection();
        if (arr->sections.empty())
        {
            g.setColour (tokens::card);
            g.fillRoundedRectangle (geo.ruler.toFloat(), 6.0f);
            g.setColour (tokens::dim);
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText ("No sections yet. Press + Section, or double-click where a section starts.",
                        geo.ruler.reduced (12, 0), juce::Justification::centredLeft, true);
        }
        for (int i = 0; i < (int) arr->sections.size(); ++i)
        {
            const int b0 = arr->sectionStartBar (i), b1 = arr->sectionEndBar (i);
            if (b1 <= b0) continue;
            auto block = juce::Rectangle<float> (xForBar (b0), (float) geo.ruler.getY(),
                                                 (float) (b1 - b0) * geo.pxPerBar, (float) geo.ruler.getHeight()).reduced (1.5f, 0.0f);
            const auto col = sectionColour (*arr, i);
            const auto& s = arr->sections[(size_t) i];
            const bool dimmed = s.skip || s.optional;
            const bool isCurrent = i == current;

            if (isCurrent)
            {
                g.setColour (col.withAlpha (0.28f));
                g.fillRoundedRectangle (block.expanded (2.0f, 2.0f), 7.0f);
                g.setGradientFill (juce::ColourGradient (col.brighter (0.25f), 0.0f, block.getY(),
                                                         col.darker (0.15f), 0.0f, block.getBottom(), false));
                g.fillRoundedRectangle (block, 5.0f);
            }
            else
            {
                g.setColour (col.withAlpha (dimmed ? 0.10f : 0.22f));
                g.fillRoundedRectangle (block, 5.0f);
                g.setColour (col.withAlpha (dimmed ? 0.35f : 0.80f));
                g.drawRoundedRectangle (block.reduced (0.5f), 5.0f, 1.0f);
            }
            if (i == queued)
            {
                g.setColour (tokens::queued);
                g.drawRoundedRectangle (block, 5.0f, 2.0f);
            }

            g.setColour (isCurrent ? juce::Colours::black.withAlpha (0.85f) : (dimmed ? tokens::faint : tokens::bright));
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            juce::String label = juce::String (s.name);
            if (s.loopOnEntry) label += " \xe2\x9f\xb3";
            if (s.pauseAfter)  label += " \xe2\x8f\xb8";
            if (s.optional)    label += " (opt)";
            g.drawFittedText (juce::String (juce::CharPointer_UTF8 (label.toRawUTF8())),
                              block.reduced (8.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        }

        // ---- lanes ----
        const float playX = xForBar (host.currentBar());
        for (int slot = 0; slot < (int) visibleLanes.size(); ++slot)
        {
            const int l = visibleLanes[(size_t) slot];
            const auto row = laneRow (slot, geo);
            drawLaneHeader (g, headerRect (row), song, l);
            drawLaneWave (g, waveRect (row, geo), song, l, playX);
        }

        // ---- bar numbers and grid ----
        const int every = geo.pxPerBar >= 28.0f ? 1 : geo.pxPerBar >= 12.0f ? 2 : geo.pxPerBar >= 6.0f ? 4 : 8;
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        for (int b = 0; b <= geo.bars; b += every)
        {
            const float x = xForBar (b);
            if (b < geo.bars)
            {
                g.setColour (tokens::faint);
                g.drawText (juce::String (b + 1), (int) x + 3, geo.barRow.getY(), 34, geo.barRow.getHeight(), juce::Justification::centredLeft);
            }
            g.setColour (tokens::bright.withAlpha (b % 4 == 0 ? 0.10f : 0.05f));
            g.fillRect (juce::Rectangle<float> (x, (float) geo.lanes.getY(), 1.0f, (float) geo.lanes.getHeight()));
        }

        // ---- loop shading ----
        if (host.isLoopingSection() && current >= 0)
        {
            const int b0 = arr->sectionStartBar (current), b1 = arr->sectionEndBar (current);
            g.setColour (tokens::queued.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float> (xForBar (b0), (float) geo.lanes.getY(), (float) (b1 - b0) * geo.pxPerBar, (float) geo.lanes.getHeight()));
        }

        // ---- where a new section would go ----
        if (hoverBar >= 0 && draggingBoundary < 0)
        {
            const float x = xForBar (hoverBar);
            g.setColour (tokens::bright.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) geo.ruler.getY(), 1.0f, (float) (geo.lanes.getBottom() - geo.ruler.getY())));
            const auto plus = juce::Rectangle<float> (x - 7.0f, (float) geo.barRow.getY() - 1.0f, 14.0f, 14.0f);
            g.setColour (tokens::indigo);
            g.fillEllipse (plus);
            g.setColour (tokens::bright);
            g.fillRect (plus.withSizeKeepingCentre (7.0f, 1.6f));
            g.fillRect (plus.withSizeKeepingCentre (1.6f, 7.0f));
        }

        // ---- playhead ----
        g.setColour (tokens::play.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (playX - 3.0f, (float) geo.ruler.getY(), 6.0f, (float) (geo.lanes.getBottom() - geo.ruler.getY())));
        g.setColour (tokens::play);
        g.fillRect (juce::Rectangle<float> (playX - 1.0f, (float) geo.ruler.getY(), 2.0f, (float) (geo.lanes.getBottom() - geo.ruler.getY())));
        juce::Path tri;
        tri.addTriangle (playX - 6.0f, (float) geo.ruler.getY() - 1.0f, playX + 6.0f, (float) geo.ruler.getY() - 1.0f,
                         playX, (float) geo.ruler.getY() + 6.0f);
        g.fillPath (tri);

        // ---- drag feedback ----
        if (draggingBoundary >= 0)
        {
            g.setColour (tokens::bright.withAlpha (0.8f));
            const float x = xForBar (dragBar);
            g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) geo.ruler.getY(), 1.0f, (float) (geo.lanes.getBottom() - geo.ruler.getY())));
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto geo = geometry();
        int hb = -1;
        juce::String tip;

        if (geo.bars > 0 && e.x >= geo.ruler.getX() && e.y >= geo.ruler.getY() && e.y < geo.lanes.getBottom())
        {
            hb = barAt (e.x, geo);
            tip = e.y < geo.barRow.getBottom()
                ? "Click a section to jump to it. Drag its left edge to move it. Double-click to add a section at bar " + juce::String (hb + 1) + "."
                : "Click to move the playhead. Double-click to add a section at bar " + juce::String (hb + 1) + ".";
        }
        else if (const int slot = laneSlotAt (e.y, geo); slot >= 0 && e.x < geo.ruler.getX())
        {
            const auto L = headerLayout (headerRect (laneRow (slot, geo)));
            const auto p = e.getPosition();
            tip = L.solo.contains (p) ? "Solo this track"
                : L.mute.contains (p) ? "Mute this track"
                                      : "Name this track (the icon follows the name)";
        }

        if (tip != getTooltip()) setTooltip (tip);
        if (hb != hoverBar) { hoverBar = hb; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverBar != -1) { hoverBar = -1; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr) return;
        const auto geo = geometry();
        if (geo.bars <= 0) return;
        const int song = host.currentSongIndex();
        const auto pos = e.getPosition();

        // ---- lane headers ----
        if (e.x < geo.ruler.getX())
        {
            const int slot = laneSlotAt (e.y, geo);
            if (slot < 0 || song < 0) return;
            const int l = visibleLanes[(size_t) slot];
            const auto L = headerLayout (headerRect (laneRow (slot, geo)));
            if (! e.mods.isPopupMenu() && L.solo.contains (pos)) { toggleSolo (song, l); return; }
            if (! e.mods.isPopupMenu() && L.mute.contains (pos)) { host.toggleLayer (song, l); repaint(); return; }
            if (onLaneMenu) onLaneMenu (l, e.getScreenPosition());
            return;
        }

        const int bar = barAt (e.x, geo);
        const int s = arr->sectionAtBar (bar);

        if (e.mods.isPopupMenu())
        {
            openContextMenu (e.y < geo.ruler.getBottom(), bar, s, e.getScreenPosition());
            return;
        }

        // Touch: a hold anywhere on the timeline opens the same menu as a
        // right-click. So that a hold never jumps the song first, jumping and
        // seeking happen when the finger LIFTS, not when it lands.
        pendingJump = -1;
        pendingSeekBar = -1;
        const bool onRuler = e.y < geo.ruler.getBottom();
        touchHold.onLongPress = [this, onRuler, bar, s] (juce::Point<int> screen)
        {
            pendingJump = -1;
            pendingSeekBar = -1;
            draggingBoundary = -1;
            repaint();
            openContextMenu (onRuler, bar, s, screen);
        };
        touchHold.begin (e);

        if (e.y < geo.barRow.getBottom())
        {
            // near a boundary? grab it for dragging (a finger is wider than a cursor)
            for (int i = 1; i < (int) arr->sections.size(); ++i)
            {
                const float x = (float) geo.ruler.getX() + (float) arr->sectionStartBar (i) * geo.pxPerBar;
                if (std::abs ((float) e.x - x) <= 10.0f) { draggingBoundary = i; dragBar = arr->sectionStartBar (i); return; }
            }
            // otherwise a tap on a block queues a jump to it, on release
            if (s >= 0 && e.getNumberOfClicks() == 1) pendingJump = s;
            return;
        }

        pendingSeekBar = bar;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        touchHold.drag (e);
        if (draggingBoundary < 0) return;
        const auto geo = geometry();
        dragBar = barAt (e.x, geo);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool wasHold = touchHold.end();
        if (! wasHold && ! e.mods.isPopupMenu() && ! e.mouseWasDraggedSinceMouseDown())
        {
            if (pendingJump >= 0)    host.jumpToSection (pendingJump);
            if (pendingSeekBar >= 0) host.seekToBar ((double) pendingSeekBar);
        }
        pendingJump = -1;
        pendingSeekBar = -1;

        if (draggingBoundary < 0) return;
        if (! wasHold)
            if (auto* arr = host.currentArrangement())
                if (arr->moveSection (draggingBoundary, dragBar))
                    host.arrangementEdited();
        draggingBoundary = -1;
        repaint();
    }

    /** The one context menu of the timeline, from right-click or a hold:
        a section's own menu on the ruler, "add a section here" on the lanes. */
    void openContextMenu (bool onRuler, int bar, int s, juce::Point<int> screen)
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr) return;
        if (onRuler && s >= 0)
        {
            if (onSectionMenu) onSectionMenu (s, screen);
            return;
        }
        juce::PopupMenu m;
        m.addItem (1, "Add section at bar " + juce::String (bar + 1));
        if (s >= 0 && s < (int) arr->sections.size())
            m.addItem (2, "Edit section \"" + juce::String (juce::CharPointer_UTF8 (arr->sections[(size_t) s].name.c_str())) + "\"");
        m.addItem (3, "Move playhead to bar " + juce::String (bar + 1));
        juce::Component::SafePointer<Timeline> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screen.x, screen.y, 1, 1)),
                         [safe, bar, s, screen] (int r)
        {
            if (safe == nullptr) return;
            if (r == 1 && safe->onAddSectionAt) safe->onAddSectionAt (bar, screen);
            if (r == 2 && safe->onSectionMenu)  safe->onSectionMenu (s, screen);
            if (r == 3) safe->host.seekToBar ((double) bar);
        });
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto geo = geometry();
        if (geo.bars <= 0 || e.x < geo.ruler.getX() || e.y < geo.ruler.getY() || e.y >= geo.lanes.getBottom()) return;
        if (onAddSectionAt) onAddSectionAt (barAt (e.x, geo), e.getScreenPosition());
    }

    void resized() override
    {
        for (auto& img : laneImages) img = juce::Image();
    }

private:
    static constexpr int kRulerHeight   = 30;
    static constexpr int kBarRowHeight  = 14;
    static constexpr int kHeaderW       = 176;
    static constexpr int kPeakRes       = 2048;

    struct Geo { juce::Rectangle<int> inner, ruler, barRow, lanes; int bars { 0 }; float pxPerBar { 0 }; };
    struct HeaderLayout { juce::Rectangle<int> icon, name, sub, solo, mute, edit; };

    Geo geometry() const
    {
        Geo g;
        const int song = host.currentSongIndex();
        g.bars = song >= 0 ? (std::max) (1, host.songLengthBars (song)) : 0;
        g.inner = getLocalBounds().reduced (12, 10);
        const auto content = g.inner.withTrimmedLeft (kHeaderW);
        g.ruler  = content.withHeight (kRulerHeight);
        g.barRow = content.withTrimmedTop (kRulerHeight).withHeight (kBarRowHeight);
        g.lanes  = g.inner.withTrimmedTop (kRulerHeight + kBarRowHeight + 2);
        g.pxPerBar = g.bars > 0 ? (float) content.getWidth() / (float) g.bars : 0.0f;
        return g;
    }

    int barAt (int x, const Geo& g) const
    {
        if (g.pxPerBar <= 0.0f) return 0;
        return juce::jlimit (0, (std::max) (0, g.bars - 1), (int) std::floor ((float) (x - g.ruler.getX()) / g.pxPerBar));
    }

    int laneSlotAt (int y, const Geo& g) const
    {
        const int count = (int) visibleLanes.size();
        if (count <= 0 || y < g.lanes.getY() || y >= g.lanes.getBottom()) return -1;
        const int laneH = (std::max) (1, g.lanes.getHeight() / count);
        const int slot = (y - g.lanes.getY()) / laneH;
        return slot < count ? slot : -1;
    }

    juce::Rectangle<int> laneRow (int slot, const Geo& g) const
    {
        const int count = (std::max) (1, (int) visibleLanes.size());
        const int laneH = g.lanes.getHeight() / count;
        return juce::Rectangle<int> (g.inner.getX(), g.lanes.getY() + slot * laneH, g.inner.getWidth(), laneH).reduced (0, 3);
    }

    static juce::Rectangle<int> headerRect (juce::Rectangle<int> row)             { return row.withWidth (kHeaderW - 8); }
    static juce::Rectangle<int> waveRect (juce::Rectangle<int> row, const Geo& g) { return row.withLeft (g.ruler.getX()); }

    /** Tall lanes (four tracks) stack S / M / pencil like the reference; short
        ones (eight tracks) put S and M side by side and the name opens the
        naming menu, so every control stays big enough to hit. */
    static HeaderLayout headerLayout (juce::Rectangle<int> box)
    {
        HeaderLayout L;
        auto b = box.reduced (8, 4).withTrimmedLeft (2);
        const int bw = 22;
        if (b.getHeight() >= 66)
        {
            auto stack = b.removeFromRight (bw).withSizeKeepingCentre (bw, 18 * 3 + 6);
            L.solo = stack.removeFromTop (18); stack.removeFromTop (3);
            L.mute = stack.removeFromTop (18); stack.removeFromTop (3);
            L.edit = stack.removeFromTop (18);
        }
        else
        {
            const int bh = (std::min) (18, b.getHeight());
            auto row = b.removeFromRight (bw * 2 + 3).withSizeKeepingCentre (bw * 2 + 3, bh);
            L.solo = row.removeFromLeft (bw); row.removeFromLeft (3);
            L.mute = row;
        }
        b.removeFromRight (6);
        const int iconSize = juce::jlimit (14, 34, b.getHeight() - 8);
        L.icon = b.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize);
        b.removeFromLeft (10);
        auto text = b.withSizeKeepingCentre (b.getWidth(), (std::min) (b.getHeight(), 32));
        L.name = text.removeFromTop (text.getHeight() * 11 / 20);
        L.sub  = text;
        return L;
    }

    void drawLaneHeader (juce::Graphics& g, juce::Rectangle<int> box, int song, int l) const
    {
        const auto col = laneColour (l);
        const bool on = host.layerEnabled (song, l);
        const bool solo = soloLane == l && soloSong == song;
        const bool live = ! hasAudio[(size_t) l] && host.layerIsLive (song, l);
        const auto name = host.layerName (song, l);

        g.setColour (tokens::card.withAlpha (0.9f));
        g.fillRoundedRectangle (box.toFloat(), 8.0f);
        g.setColour (col.withAlpha (on ? 0.95f : 0.30f));
        g.fillRoundedRectangle (box.toFloat().removeFromLeft (3.0f).reduced (0.0f, 6.0f), 1.5f);

        const auto L = headerLayout (box);
        drawStemIcon (g, stemKindFor (name), L.icon.toFloat(), on ? col : tokens::faint);

        g.setColour (on ? col.brighter (0.2f) : tokens::faint);
        g.setFont (juce::Font (juce::FontOptions (juce::jlimit (10.0f, 13.0f, (float) L.name.getHeight() * 0.8f), juce::Font::bold))
                       .withExtraKerningFactor (0.03f));
        g.drawFittedText (name.toUpperCase(), L.name, juce::Justification::bottomLeft, 1, 0.8f);

        g.setColour (solo ? tokens::queued : (on ? tokens::dim : tokens::danger.withAlpha (0.85f)));
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)).withExtraKerningFactor (0.08f));
        g.drawText (solo ? "SOLO" : (! on ? "MUTED" : (live ? "LIVE" : "STEM")), L.sub, juce::Justification::topLeft, true);

        drawMiniButton (g, L.solo, "S", solo, tokens::queued);
        drawMiniButton (g, L.mute, "M", ! on, tokens::danger);
        if (! L.edit.isEmpty()) drawPencil (g, L.edit, tokens::dim);
    }

    static void drawMiniButton (juce::Graphics& g, juce::Rectangle<int> rect, const char* text, bool active, juce::Colour activeColour)
    {
        if (rect.isEmpty()) return;
        const auto f = rect.toFloat();
        g.setColour (active ? activeColour : tokens::shell);
        g.fillRoundedRectangle (f, 4.0f);
        g.setColour (active ? activeColour.brighter (0.3f) : tokens::border);
        g.drawRoundedRectangle (f.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (active ? juce::Colours::black.withAlpha (0.85f) : tokens::dim);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText (text, rect, juce::Justification::centred);
    }

    static void drawPencil (juce::Graphics& g, juce::Rectangle<int> rect, juce::Colour c)
    {
        const auto f = rect.toFloat().reduced (3.5f);
        juce::Path p;
        p.startNewSubPath (f.getX() + f.getWidth() * 0.22f, f.getBottom() - f.getHeight() * 0.22f);
        p.lineTo (f.getRight() - f.getWidth() * 0.12f, f.getY() + f.getHeight() * 0.12f);
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        juce::Path tip;
        tip.addTriangle (f.getX(), f.getBottom(),
                         f.getX() + f.getWidth() * 0.30f, f.getBottom() - f.getHeight() * 0.06f,
                         f.getX() + f.getWidth() * 0.06f, f.getBottom() - f.getHeight() * 0.30f);
        g.fillPath (tip);
    }

    void drawLaneWave (juce::Graphics& g, juce::Rectangle<int> box, int song, int l, float playX)
    {
        const auto col = laneColour (l);
        const bool on = host.layerEnabled (song, l);
        g.setColour (juce::Colour (0xff0b0b18u));
        g.fillRoundedRectangle (box.toFloat(), 6.0f);
        if (box.getWidth() < 4 || box.getHeight() < 6) return;

        if (! hasAudio[(size_t) l])
        {
            g.setColour (tokens::border.withAlpha (0.6f));
            g.fillRect (juce::Rectangle<float> ((float) box.getX() + 8.0f, (float) box.getCentreY(), (float) box.getWidth() - 16.0f, 1.0f));
            g.setColour (tokens::faint);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText (host.layerIsLive (song, l) ? "Live -- plays from its input or your MIDI keyboard" : "No audio on this track",
                        box.reduced (10, 0), juce::Justification::centredLeft, true);
            return;
        }

        auto& img = laneImages[(size_t) l];
        if (! img.isValid() || img.getWidth() != box.getWidth() || img.getHeight() != box.getHeight())
            img = renderWave (l, box.getWidth(), box.getHeight(), col);

        const int split = juce::jlimit (box.getX(), box.getRight(), juce::roundToInt (playX));
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (box.withRight (split));
            g.setOpacity (on ? 1.0f : 0.20f);
            g.drawImageAt (img, box.getX(), box.getY());
        }
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (box.withLeft (split));
            g.setOpacity (on ? 0.55f : 0.14f);
            g.drawImageAt (img, box.getX(), box.getY());
        }

        if (box.getHeight() >= 40)
        {
            const auto file = host.layerFileName (song, l);
            if (file.isNotEmpty() && file.compareIgnoreCase (host.layerName (song, l)) != 0)
            {
                g.setColour (tokens::bright.withAlpha (on ? 0.70f : 0.35f));
                g.setFont (juce::Font (juce::FontOptions (10.0f)));
                g.drawText (file, box.reduced (8, 4).removeFromTop (12), juce::Justification::centredLeft, true);
            }
        }
    }

    juce::Image renderWave (int l, int w, int h, juce::Colour col) const
    {
        w = (std::max) (1, w);
        h = (std::max) (1, h);
        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);

        const float mid  = (float) h * 0.5f;
        const float half = (float) h * 0.42f;
        const auto& pk = peaks[(size_t) l];
        const auto& rm = rms[(size_t) l];

        auto at = [w] (const std::vector<float>& v, int x)
        {
            const int b0 = juce::jlimit (0, kPeakRes - 1, (int) ((double) x / w * kPeakRes));
            const int b1 = juce::jlimit (b0 + 1, kPeakRes, (int) ((double) (x + 1) / w * kPeakRes));
            float m = 0.0f;
            for (int b = b0; b < b1; ++b) m = (std::max) (m, v[(size_t) b]);
            return m;
        };

        std::vector<float> body ((size_t) w), core ((size_t) w);
        for (int x = 0; x < w; ++x)
        {
            body[(size_t) x] = (std::min) (1.0f, at (pk, x)) * half;
            core[(size_t) x] = (std::min) (1.0f, at (rm, x)) * half;
        }

        auto outline = [w, mid] (const std::vector<float>& t)
        {
            juce::Path p;
            p.startNewSubPath (0.0f, mid - t[0]);
            for (int x = 1; x < w; ++x) p.lineTo ((float) x, mid - t[(size_t) x]);
            for (int x = w - 1; x >= 0; --x) p.lineTo ((float) x, mid + t[(size_t) x]);
            p.closeSubPath();
            return p;
        };
        const auto bodyPath = outline (body);
        const auto corePath = outline (core);

        g.setColour (col.withAlpha (0.16f));
        g.strokePath (bodyPath, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::ColourGradient grad (col.withAlpha (0.35f), 0.0f, mid - half, col.withAlpha (0.35f), 0.0f, mid + half, false);
        grad.addColour (0.5, col.withAlpha (0.80f));
        g.setGradientFill (grad);
        g.fillPath (bodyPath);

        g.setColour (col.brighter (0.45f).withAlpha (0.95f));
        g.fillPath (corePath);

        g.setColour (col.withAlpha (0.45f));
        g.fillRect (0.0f, mid - 0.5f, (float) w, 1.0f);
        return img;
    }

    void toggleSolo (int song, int l)
    {
        auto setEnabled = [this, song] (int layer, bool on)
        {
            if (host.layerEnabled (song, layer) != on) host.toggleLayer (song, layer);
        };
        if (soloSong == song && soloLane == l)
        {
            for (int i = 0; i < ezdeck::kNumLayers; ++i) setEnabled (i, preSolo[(size_t) i]);
            soloLane = soloSong = -1;
        }
        else
        {
            if (soloSong != song || soloLane < 0)
                for (int i = 0; i < ezdeck::kNumLayers; ++i) preSolo[(size_t) i] = host.layerEnabled (song, i);
            for (int i = 0; i < ezdeck::kNumLayers; ++i) setEnabled (i, i == l);
            soloLane = l;
            soloSong = song;
        }
        repaint();
    }

    PlaybackHost& host;
    std::array<std::vector<float>, ezdeck::kNumLayers> peaks, rms;
    std::array<bool, ezdeck::kNumLayers> hasAudio {};
    std::array<juce::Image, ezdeck::kNumLayers> laneImages;
    // PX-B: the lanes actually drawn, in order. A song with four stems
    // still looks like a four-stem song -- an empty layer is not given a
    // lane, because a row of blank lanes on a music stand is noise.
    std::vector<int> visibleLanes;
    int draggingBoundary { -1 };
    int dragBar { 0 };
    int hoverBar { -1 };
    eztouch::LongPress touchHold;
    int pendingJump { -1 };       // a tap on a section block jumps on release
    int pendingSeekBar { -1 };    // a tap on the lanes seeks on release
    // Solo is the view's own: it remembers which tracks were on, turns the
    // others off, and puts them back exactly as they were when released.
    int soloLane { -1 }, soloSong { -1 };
    std::array<bool, ezdeck::kNumLayers> preSolo {};
};

//==============================================================================
//  The setlist on the left.
//==============================================================================
class SetlistPanel : public juce::Component, private juce::ListBoxModel
{
public:
    explicit SetlistPanel (PlaybackHost& h) : host (h)
    {
        title.setText ("SETLIST", juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, tokens::bright);
        title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        addAndMakeVisible (title);

        list.setModel (this);
        list.setRowHeight (48);
        list.addMouseListener (this, true);   // touch: taps and holds on rows, see mouseDown/mouseUp below
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);

        addButton.setButtonText ("+ Add song");
        addButton.onClick = [this] { host.promptAddSong(); };
        addAndMakeVisible (addButton);

        removeButton.setButtonText ("Remove");
        removeButton.onClick = [this] { const int r = list.getSelectedRow(); if (r >= 0) host.removeSong (r); };
        addAndMakeVisible (removeButton);

        upButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb2")));
        upButton.onClick = [this] { const int r = list.getSelectedRow(); if (r > 0) { host.moveSong (r, r - 1); list.selectRow (r - 1); } };
        addAndMakeVisible (upButton);

        downButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbc")));
        downButton.onClick = [this] { const int r = list.getSelectedRow(); if (r >= 0 && r + 1 < host.numSongs()) { host.moveSong (r, r + 1); list.selectRow (r + 1); } };
        addAndMakeVisible (downButton);

        totalLabel.setColour (juce::Label::textColourId, tokens::faint);
        totalLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        totalLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (totalLabel);
    }

    void refresh()
    {
        list.updateContent();
        const int cur = host.currentSongIndex();
        if (cur >= 0 && list.getSelectedRow() != cur) list.selectRow (cur, true, true);
        double total = 0.0;
        for (int i = 0; i < host.numSongs(); ++i) total += host.songLengthSeconds (i);
        totalLabel.setText (juce::String (host.numSongs()) + (host.numSongs() == 1 ? " song  " : " songs  ") + formatClock (total),
                            juce::dontSendNotification);
        list.repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (tokens::card);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        auto top = r.removeFromTop (22);
        title.setBounds (top.removeFromLeft (90));
        totalLabel.setBounds (top);
        r.removeFromTop (6);
        auto foot = r.removeFromBottom (28);
        addButton.setBounds (foot.removeFromLeft (96)); foot.removeFromLeft (6);
        removeButton.setBounds (foot.removeFromLeft (70)); foot.removeFromLeft (6);
        downButton.setBounds (foot.removeFromRight (30)); foot.removeFromRight (4);
        upButton.setBounds (foot.removeFromRight (30));
        r.removeFromBottom (6);
        list.setBounds (r);
    }

private:
    int getNumRows() override { return host.numSongs(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        const bool current = row == host.currentSongIndex();
        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (2, 2);
        g.setColour (current ? tokens::indigo.withAlpha (0.22f) : (selected ? tokens::border.withAlpha (0.5f) : juce::Colours::transparentBlack));
        g.fillRoundedRectangle (r.toFloat(), 8.0f);

        auto accent = r.removeFromLeft (5);
        g.setColour (host.songColour (row));
        g.fillRoundedRectangle (accent.toFloat().reduced (0, 6), 2.0f);
        r.removeFromLeft (10);

        g.setColour (tokens::faint);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (juce::String (row + 1), r.removeFromLeft (22), juce::Justification::centredLeft);

        auto right = r.removeFromRight (54);
        g.setColour (tokens::faint);
        g.drawText (formatClock (host.songLengthSeconds (row)), right, juce::Justification::centredRight);

        g.setColour (host.songHasAudio (row) ? tokens::bright : tokens::faint);
        g.setFont (juce::Font (juce::FontOptions (13.0f, current ? juce::Font::bold : juce::Font::plain)));
        g.drawText (host.songName (row), r.removeFromTop (h / 2 + 2), juce::Justification::bottomLeft, true);
        g.setColour (tokens::faint);
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawText (host.songHasAudio (row) ? juce::String (host.songLengthBars (row)) + " bars" : juce::String ("no audio"),
                    r, juce::Justification::topLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        // Selecting a song switches the music, so it happens on RELEASE (see
        // mouseUp): JUCE calls this on press, and a hold for the row menu
        // must never change the song first.
        if (e.mods.isPopupMenu()) showRowMenu (row, e.getScreenPosition());
    }

    void showRowMenu (int row, juce::Point<int> screen)
    {
        if (row < 0 || row >= host.numSongs()) return;
        juce::PopupMenu m;
        m.addSectionHeader (host.songName (row));
        m.addItem (1, "Rename...");
        m.addItem (3, "Move up", row > 0);
        m.addItem (4, "Move down", row + 1 < host.numSongs());
        m.addSeparator();
        m.addItem (2, "Remove from setlist");
        juce::Component::SafePointer<SetlistPanel> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screen.x, screen.y, 1, 1)),
                         [safe, row] (int r)
        {
            if (safe == nullptr) return;
            if (r == 1) safe->host.renameSong (row);
            if (r == 2) safe->host.removeSong (row);
            if (r == 3) { safe->host.moveSong (row, row - 1); safe->list.selectRow (row - 1); }
            if (r == 4) { safe->host.moveSong (row, row + 1); safe->list.selectRow (row + 1); }
        });
    }

    int rowAt (const juce::MouseEvent& e)
    {
        const auto p = e.getEventRelativeTo (&list).getPosition();
        return list.getRowContainingPosition (p.x, p.y);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this || e.mods.isPopupMenu()) return;
        pressedRow = rowAt (e);
        if (pressedRow < 0) return;
        rowHold.onLongPress = [this, row = pressedRow] (juce::Point<int> screen) { showRowMenu (row, screen); };
        rowHold.begin (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.eventComponent != this) rowHold.drag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this || e.mods.isPopupMenu()) return;
        const bool wasHold = rowHold.end();
        const int row = rowAt (e);
        if (! wasHold && row >= 0 && row == pressedRow && ! e.mouseWasDraggedSinceMouseDown())
            host.selectSong (row);
        pressedRow = -1;
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        host.selectSong (row);
        if (! host.isPlaying()) host.playStop();
    }

    PlaybackHost& host;
    juce::Label title, totalLabel;
    juce::ListBox list;
    juce::TextButton addButton, removeButton, upButton, downButton;
    eztouch::LongPress rowHold;
    int pressedRow { -1 };
};

//==============================================================================
//  The view.
//==============================================================================
class PlaybackView : public juce::Component, private juce::Timer
{
public:
    explicit PlaybackView (PlaybackHost& h) : host (h), timeline (h), setlist (h)
    {
        addAndMakeVisible (setlist);
        addAndMakeVisible (timeline);

        timeline.onSectionMenu  = [this] (int s, juce::Point<int> p) { showSectionMenu (s, p); };
        timeline.onAddSectionAt = [this] (int bar, juce::Point<int> p) { addSectionAt (bar, p); };
        timeline.onLaneMenu     = [this] (int layer, juce::Point<int> p) { showLaneMenu (layer, p); };

        auto mk = [this] (juce::TextButton& b, const char* text, std::function<void()> fn)
        {
            b.setButtonText (juce::String (juce::CharPointer_UTF8 (text)));
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };
        mk (prevSongButton,    "\xe2\x8f\xae",            [this] { host.prevSong(); });
        mk (prevSectionButton, "\xe2\x97\x80 Section",    [this] { host.prevSection(); });
        mk (playButton,        "\xe2\x96\xb6 Play",       [this] { host.playStop(); });
        mk (countInButton,     "Count-in \xe2\x96\xb6",   [this] { host.playWithCountIn(); });
        mk (nextSectionButton, "Section \xe2\x96\xb6",    [this] { host.nextSection(); });
        mk (nextSongButton,    "\xe2\x8f\xad",            [this] { host.nextSong(); });
        mk (loopButton,        "Loop section",            [this] { host.toggleLoopSection(); });
        mk (jumpNowButton,     "Jump now",                [this] { host.jumpNow(); });
        mk (cancelButton,      "Cancel",                  [this] { host.cancelJump(); });
        mk (autoSectionButton, "Auto-section",            [this] { autoSection(); });
        mk (addSectionButton,  "+ Section",               [this] { addSectionAtPlayhead(); });
        addSectionButton.setTooltip ("Add a section where the playhead is");
        mk (clickButton,       "Click",                   [this] { host.setGuideClick (! host.guideClick()); songChanged(); });
        clickButton.setTooltip ("Native click for this song, on the CLICK mixer strip (route it to the drummer's ears)");
        mk (cuesButton,        "Cues",                    [this] { cuesMenu(); });
        cuesButton.setTooltip ("Spoken cues for this song, on the CUES mixer strip: the section name a couple of bars early, then a count");

        jumpModeBox.addItem ("Jump: next bar", 1);
        jumpModeBox.addItem ("Jump: end of section", 2);
        jumpModeBox.addItem ("Jump: immediately", 3);
        jumpModeBox.onChange = [this] { host.setJumpMode (jumpModeBox.getSelectedId() - 1); };
        addAndMakeVisible (jumpModeBox);

        countInBox.addItem ("No count-in", 1);
        countInBox.addItem ("1 bar", 2);
        countInBox.addItem ("2 bars", 3);
        countInBox.addItem ("4 bars", 5);
        countInBox.onChange = [this] { host.setCountInBars (countInBox.getSelectedId() - 1); };
        addAndMakeVisible (countInBox);

        startTimerHz (30);
    }

    ~PlaybackView() override { stopTimer(); }

    /** Call when the song/setlist/arrangement changed (not every frame). */
    void songChanged()
    {
        timeline.rebuildPeaks();
        setlist.refresh();
        jumpModeBox.setSelectedId (host.jumpMode() + 1, juce::dontSendNotification);
        countInBox.setSelectedId (host.countInBars() + 1, juce::dontSendNotification);
        const bool haveSong = host.currentArrangement() != nullptr;
        clickButton.setEnabled (haveSong);
        cuesButton.setEnabled (haveSong && host.cueBankLoaded());
        clickButton.setColour (juce::TextButton::buttonColourId, host.guideClick() ? tokens::queued.withAlpha (0.85f) : tokens::card);
        clickButton.setColour (juce::TextButton::textColourOffId, host.guideClick() ? juce::Colours::black : tokens::bright);
        cuesButton.setColour (juce::TextButton::buttonColourId, host.guideCues() ? tokens::ringA.withAlpha (0.85f) : tokens::card);
        cuesButton.setColour (juce::TextButton::textColourOffId, host.guideCues() ? juce::Colours::black : tokens::bright);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tokens::shell);

        auto* arr = host.currentArrangement();
        const int song = host.currentSongIndex();
        const auto hdr = headerArea;

        // ---- NOW / NEXT ----
        const int cur = host.currentSection();
        const int queued = host.queuedSection();
        int next = -1;
        if (arr != nullptr)
            next = queued >= 0 ? queued : arr->nextPlayableAfter (cur);

        // NOW and NEXT now flank the circular readout rather than being
        // crowded into the left two thirds with a flat panel beside them.
        auto hdrRow = hdr;
        const int dialSize = (std::min) (hdrRow.getHeight(), 172);
        const int flank    = (std::max) (120, (hdrRow.getWidth() - dialSize) / 2);
        auto nowBox  = hdrRow.removeFromLeft (flank).reduced (0, 4).withTrimmedRight (10);
        auto dialBox = hdrRow.removeFromLeft ((std::min) (dialSize, hdrRow.getWidth()));
        auto nextBox = hdrRow.reduced (0, 4).withTrimmedLeft (10);

        auto drawTag = [&] (juce::Rectangle<int> box, const char* label, int section, bool isQueued)
        {
            g.setColour (tokens::card);
            g.fillRoundedRectangle (box.toFloat(), 10.0f);
            auto col = arr != nullptr && section >= 0 ? sectionColour (*arr, section) : tokens::faint;
            g.setColour (col.withAlpha (0.9f));
            g.fillRoundedRectangle (box.toFloat().removeFromLeft (6.0f), 3.0f);
            if (isQueued) { g.setColour (tokens::queued); g.drawRoundedRectangle (box.toFloat(), 10.0f, 1.5f); }
            auto inner = box.reduced (16, 8);
            g.setColour (tokens::faint);
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (label, inner.removeFromTop (14), juce::Justification::topLeft);
            g.setColour (tokens::bright);
            g.setFont (juce::Font (juce::FontOptions ((float) juce::jlimit (18, 34, inner.getHeight() - 6), juce::Font::bold)));
            std::string name = "\xe2\x80\x94";
            if (arr != nullptr && section >= 0 && section < (int) arr->sections.size()) name = arr->sections[(size_t) section].name;
            else if (arr != nullptr && section < 0 && cur >= 0 && host.numSongs() > host.currentSongIndex() + 1 && label[1] == 'E')
                name = "\xe2\x86\x92 " + host.songName (host.currentSongIndex() + 1).toStdString();
            drawNeonTitle (g, inner, juce::String (juce::CharPointer_UTF8 (name.c_str())), col, section >= 0);
        };
        drawTag (nowBox, "NOW", cur, false);
        drawTag (nextBox, queued >= 0 ? "NEXT (QUEUED)" : "NEXT", next, queued >= 0);

        // ---- the circular musical readout --------------------------------
        const double bar = host.currentBar();
        const int beatsPerBar = (std::max) (1, host.beatsPerBar());
        const int barNo = (int) std::floor (bar) + 1;
        const int beatNo = (int) std::floor ((bar - std::floor (bar)) * beatsPerBar) + 1;

        double remaining = 0.0;
        if (song >= 0) remaining = (std::max) (0.0, (host.songLengthBars (song) - bar) * host.secondsPerBar());
        double setLeft = remaining;
        for (int i = (std::max) (0, song + 1); i < host.numSongs(); ++i) setLeft += host.songLengthSeconds (i);

        // How far round the ring is lit, and why:
        //
        //   resting  -- progress through the CURRENT SECTION, which is what the
        //               big NOW name beside it refers to.
        //   queued   -- progress to the boundary the jump will actually fire
        //               on, which depends on the jump mode. That is the moment
        //               an MD most needs a countdown for, so the ring answers
        //               the question being asked rather than the general one.
        //   count-in -- progress through the bar being counted.
        //
        // The state is never carried by the ring alone: bar|beat and the
        // remaining clock sit inside it, and NEXT is tagged QUEUED.
        double sectionProgress = 0.0;
        if (arr != nullptr && cur >= 0)
        {
            const double s0 = arr->sectionStartBar (cur), s1 = arr->sectionEndBar (cur);
            sectionProgress = s1 > s0 ? juce::jlimit (0.0, 1.0, (bar - s0) / (s1 - s0)) : 0.0;
        }

        const double barPhase = bar - std::floor (bar);
        double ringProgress = sectionProgress;
        bool   ringAlert    = false;

        if (host.isCountingIn())            { ringProgress = barPhase; ringAlert = true; }
        else if (queued >= 0)
        {
            ringAlert = true;
            const int jm = host.jumpMode();
            if      (jm == 0) ringProgress = barPhase;          // fires on the next bar
            else if (jm == 1) ringProgress = sectionProgress;   // fires at the section end
            else              ringProgress = 1.0;               // fires immediately
        }

        drawDial (g, dialBox, song, barNo, beatNo, beatsPerBar,
                  remaining, setLeft, ringProgress, ringAlert, host.isCountingIn());

        // ---- visual metronome: beat dots ----
        auto dots = dotsArea.withSizeKeepingCentre (dialSize, dotsArea.getHeight());
        const int dotW = dots.getWidth() / (std::max) (1, beatsPerBar);
        for (int b = 0; b < beatsPerBar; ++b)
        {
            // circles, not pills -- four fat lozenges under the dial read as
            // unfilled slots rather than as beats.
            const int  dia  = (std::min) (dots.getHeight(), dotW) - 2;
            auto cell = juce::Rectangle<int> (dots.getX() + b * dotW, dots.getY(), dotW, dots.getHeight());
            auto dd   = cell.withSizeKeepingCentre (dia, dia).toFloat();
            const bool on = host.isPlaying() && (b + 1) == beatNo;
            auto col = arr != nullptr && cur >= 0 ? sectionColour (*arr, cur) : tokens::indigo;
            // beat one is marked even when it is not the current beat, so the
            // downbeat is findable at a glance rather than only on the hit.
            g.setColour (on ? col : (b == 0 ? tokens::faint : tokens::border));
            g.fillEllipse (dd);
        }
    }



    /** The NOW / NEXT name: condensed, filled bright-to-deep in the section's
        colour, with a soft glow -- readable from the back of a stage. */
    static void drawNeonTitle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, juce::Colour col, bool lit)
    {
        if (area.getWidth() < 20 || area.getHeight() < 12 || text.isEmpty()) return;
        const float size = (float) juce::jlimit (18, 72, area.getHeight());
        const auto font = juce::Font (juce::FontOptions (size, juce::Font::bold)).withHorizontalScale (0.88f);
        juce::GlyphArrangement ga;
        ga.addFittedText (font, text, (float) area.getX(), (float) area.getY(), (float) area.getWidth(), (float) area.getHeight(),
                          juce::Justification::centredLeft, 1, 0.55f);
        juce::Path p;
        ga.createPath (p);
        if (! lit)
        {
            g.setColour (tokens::dim);
            g.fillPath (p);
            return;
        }
        const auto b = p.getBounds();
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (col.withAlpha (0.07f * (float) (4 - i)));
            g.strokePath (p, juce::PathStrokeType ((float) i * 2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        juce::ColourGradient grad (col.brighter (0.8f), 0.0f, b.getY(), col.darker (0.35f), 0.0f, b.getBottom(), false);
        grad.addColour (0.5, col.brighter (0.15f));
        g.setGradientFill (grad);
        g.fillPath (p);
    }

    //==========================================================================
    //  The circular musical readout.
    //
    //  Drawn, not blitted. A ring lit to an arbitrary fraction cannot come
    //  from a PNG without either resampling it or shipping one asset per
    //  state, and every other surface in this app is already drawn in code --
    //  so this stays crisp at any DPI and costs no image cache.
    //
    //  Sixty segments, one full turn clockwise from twelve o'clock. The hue
    //  sweeps cyan -> violet -> magenta round the ring: that is decoration,
    //  not information, and it is deliberately the same three hues the deck
    //  lanes use so the app keeps one palette. What IS information is how far
    //  round the lit segments reach, and the numbers in the middle.
    //==========================================================================
    void drawDial (juce::Graphics& g, juce::Rectangle<int> box, int song,
                   int barNo, int beatNo, int beatsPerBar,
                   double remaining, double setLeft,
                   double progress, bool alert, bool countingIn) const
    {
        if (box.getWidth() < 40 || box.getHeight() < 40) return;

        // The set clock is taken off the bottom BEFORE the circle is measured,
        // so the ring stays centred in what is left rather than being pushed up.
        auto under = box.removeFromBottom ((int) ((float) box.getHeight() * 0.10f));

        const float d  = (float) (std::min) (box.getWidth(), box.getHeight());
        const auto  c  = box.toFloat().getCentre();
        const float cx = c.x, cy = c.y;

        // ---- housing ----
        // Bezel, recessed LED channel, face gradient, glass and inner
        // shadow all arrive as one bitmap: none of it changes, and none of
        // it is cheap to redraw at the UI tick rate. If the art folder is
        // missing we draw a plain face instead rather than showing nothing.
        const auto& faceArt = performart::dialFace();
        if (faceArt.isValid())
        {
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.drawImage (faceArt, juce::Rectangle<float> (cx - d * 0.5f, cy - d * 0.5f, d, d),
                         juce::RectanglePlacement::centred);
        }
        else
        {
            g.setColour (tokens::card);
            g.fillEllipse (cx - d * 0.42f, cy - d * 0.42f, d * 0.84f, d * 0.84f);
            g.setColour (tokens::border.withAlpha (0.55f));
            g.drawEllipse (cx - d * 0.495f, cy - d * 0.495f, d * 0.99f, d * 0.99f, 1.0f);
            g.setColour (tokens::border.withAlpha (0.30f));
            g.drawEllipse (cx - d * 0.465f, cy - d * 0.465f, d * 0.93f, d * 0.93f, 1.0f);
        }

        // ---- segmented ring ----
        const int   kSegs  = 60;
        const float radius = d * 0.435f;
        const float segW   = (std::max) (2.0f, d * 0.024f);
        const float segL   = (std::max) (4.0f, d * 0.060f);
        const float lit    = (float) juce::jlimit (0.0, 1.0, progress);

        for (int i = 0; i < kSegs; ++i)
        {
            const float t = (float) i / (float) kSegs;

            juce::Colour hue = alert
                ? tokens::queued
                : (t < 0.6f ? tokens::ringA.interpolatedWith (tokens::ringB, t / 0.6f)
                            : tokens::ringB.interpolatedWith (tokens::ringC, (t - 0.6f) / 0.4f));

            // A segment counts as lit once progress reaches its LEADING edge,
            // so the first one lights the instant playback starts rather than
            // one sixtieth of a section later.
            const bool on = t < lit || (i == 0 && lit > 0.0f);

            const auto xf = juce::AffineTransform::rotation (juce::MathConstants<float>::twoPi * t)
                                                   .translated (cx, cy);

            // THE JUICE. This is the half that cannot be baked: a lit LED
            // blooms into the groove around it, and where the lit run ENDS
            // the last few segments fall off in brightness so the ring reads
            // as something travelling rather than a bar that simply got
            // longer. Both depend on a fraction only known at paint time.
            if (on)
            {
                const float fromHead = (lit - t) / (std::max) (0.0001f, lit);
                const float heat     = juce::jlimit (0.35f, 1.0f, 1.0f - fromHead * 0.65f);

                for (int b = 3; b >= 1; --b)
                {
                    const float grow = (float) b * segW * 0.85f;
                    juce::Path halo;
                    halo.addRoundedRectangle (-(segW + grow) * 0.5f, -radius - (segL + grow) * 0.5f,
                                              segW + grow, segL + grow, (segW + grow) * 0.5f);
                    g.setColour (hue.withAlpha (0.18f * heat / (float) b));
                    g.fillPath (halo, xf);
                }
            }

            g.setColour (on ? hue.brighter (0.15f) : hue.withAlpha (0.20f));

            juce::Path seg;
            seg.addRoundedRectangle (-segW * 0.5f, -radius - segL * 0.5f, segW, segL, segW * 0.5f);
            g.fillPath (seg, xf);
        }

        // ---- twelve o'clock index ----
        juce::Path pointer;
        pointer.addTriangle (cx - d * 0.022f, cy - d * 0.525f,
                             cx + d * 0.022f, cy - d * 0.525f,
                             cx,              cy - d * 0.455f);
        g.setColour (tokens::bright);
        g.fillPath (pointer);

        // ---- centre stack ----
        const bool have = song >= 0;
        const juce::String emDash (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
        const juce::String mono = juce::Font::getDefaultMonospacedFontName();
        auto inner = box.withSizeKeepingCentre ((int) (d * 0.62f), (int) (d * 0.64f));

        // bar | beat
        auto barBeat = inner.removeFromTop ((int) (d * 0.22f));
        g.setColour (countingIn ? tokens::queued : tokens::bright);
        g.setFont (juce::Font (juce::FontOptions (mono, d * 0.19f, juce::Font::bold)));
        const int gap = (int) (d * 0.055f);
        g.drawText (have ? juce::String (barNo) : emDash,
                    barBeat.withTrimmedRight (barBeat.getWidth() / 2 + gap),
                    juce::Justification::centredRight, false);
        g.drawText (have ? juce::String (beatNo) : emDash,
                    barBeat.withTrimmedLeft (barBeat.getWidth() / 2 + gap),
                    juce::Justification::centredLeft, false);
        // the divider, drawn rather than typed so it cannot drift with the font
        g.setColour (tokens::dim.withAlpha (0.55f));
        g.drawLine ((float) barBeat.getCentreX(), (float) barBeat.getY() + d * 0.025f,
                    (float) barBeat.getCentreX(), (float) barBeat.getBottom() - d * 0.025f, 1.2f);

        g.setColour (tokens::faint);
        g.setFont (juce::Font (juce::FontOptions (d * 0.052f, juce::Font::bold)));
        auto barBeatLabel = inner.removeFromTop ((int) (d * 0.075f));
        g.drawText ("BAR", barBeatLabel.withTrimmedRight (barBeatLabel.getWidth() / 2 + gap),
                    juce::Justification::centredRight, false);
        g.drawText ("BEAT", barBeatLabel.withTrimmedLeft (barBeatLabel.getWidth() / 2 + gap),
                    juce::Justification::centredLeft, false);

        auto rule = [&] (juce::Rectangle<int> row)
        {
            g.setColour (tokens::border.withAlpha (0.7f));
            g.drawLine ((float) row.getX(), (float) row.getCentreY(),
                        (float) row.getRight(), (float) row.getCentreY(), 1.0f);
        };
        rule (inner.removeFromTop ((int) (d * 0.04f)));

        // tempo + signature
        auto tempoRow = inner.removeFromTop ((int) (d * 0.10f));
        const int tempoSplit = tempoRow.getWidth() * 3 / 5;
        g.setColour (tokens::dim);
        g.setFont (juce::Font (juce::FontOptions (mono, d * 0.062f, juce::Font::plain)));
        g.drawText (juce::String (host.tempoBpm(), 1) + " BPM",
                    tempoRow.withTrimmedRight (tempoRow.getWidth() - tempoSplit + 3),
                    juce::Justification::centredRight, false);
        g.drawText (juce::String (beatsPerBar) + "/4",
                    tempoRow.withTrimmedLeft (tempoSplit + 3),
                    juce::Justification::centredLeft, false);
        g.setColour (tokens::border);
        g.drawLine ((float) (tempoRow.getX() + tempoSplit), (float) tempoRow.getY() + 1.0f,
                    (float) (tempoRow.getX() + tempoSplit), (float) tempoRow.getBottom() - 1.0f, 1.0f);

        rule (inner.removeFromTop ((int) (d * 0.04f)));

        // remaining
        g.setColour (countingIn ? tokens::queued : tokens::bright);
        g.setFont (juce::Font (juce::FontOptions (mono, d * 0.115f, juce::Font::bold)));
        g.drawText (countingIn ? juce::String ("COUNT-IN")
                               : (have ? "-" + formatClock (remaining) : emDash),
                    inner.removeFromTop ((int) (d * 0.135f)), juce::Justification::centred, false);

        g.setColour (tokens::faint);
        g.setFont (juce::Font (juce::FontOptions (d * 0.052f, juce::Font::bold)));
        g.drawText ("REMAINING", inner.removeFromTop ((int) (d * 0.07f)),
                    juce::Justification::centred, false);

        // The whole-set clock used to live in the flat readout panel. It does
        // not fit inside the ring, so it sits just under it rather than being
        // dropped -- an MD plans the set from this number.
        g.setColour (tokens::faint);
        g.setFont (juce::Font (juce::FontOptions (mono, (std::max) (9.0f, d * 0.052f), juce::Font::plain)));
        g.drawText ("SET " + formatClock (setLeft) + " LEFT", under, juce::Justification::centred, false);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 12);
        setlist.setBounds (r.removeFromLeft (juce::jmin (280, r.getWidth() / 4)));
        r.removeFromLeft (12);

        // 172 not 92: the dial needs to be legible from a music stand,
        // and it absorbs the old separate progress bar rather than
        // stacking beside it.
        headerArea = r.removeFromTop ((std::min) (172, (std::max) (92, r.getHeight() / 4)));
        r.removeFromTop (8);
        dotsArea = r.removeFromTop (14);
        r.removeFromTop (10);

        auto transport = r.removeFromBottom (36);
        r.removeFromBottom (8);
        auto edit = r.removeFromBottom (28);
        r.removeFromBottom (8);

        timeline.setBounds (r);

        // transport row
        auto t = transport;
        prevSongButton.setBounds (t.removeFromLeft (40)); t.removeFromLeft (4);
        prevSectionButton.setBounds (t.removeFromLeft (96)); t.removeFromLeft (8);
        playButton.setBounds (t.removeFromLeft (110)); t.removeFromLeft (4);
        countInButton.setBounds (t.removeFromLeft (100)); t.removeFromLeft (8);
        nextSectionButton.setBounds (t.removeFromLeft (96)); t.removeFromLeft (4);
        nextSongButton.setBounds (t.removeFromLeft (40)); t.removeFromLeft (16);
        cancelButton.setBounds (t.removeFromRight (80)); t.removeFromRight (4);
        jumpNowButton.setBounds (t.removeFromRight (90)); t.removeFromRight (8);
        loopButton.setBounds (t.removeFromRight (110));

        // edit row
        auto e = edit;
        addSectionButton.setBounds (e.removeFromLeft (110)); e.removeFromLeft (8);
        autoSectionButton.setBounds (e.removeFromLeft (110)); e.removeFromLeft (16);
        clickButton.setBounds (e.removeFromLeft (70)); e.removeFromLeft (6);
        cuesButton.setBounds (e.removeFromLeft (70)); e.removeFromLeft (8);
        countInBox.setBounds (e.removeFromRight (120)); e.removeFromRight (8);
        jumpModeBox.setBounds (e.removeFromRight (180));
    }

private:
    void timerCallback() override
    {
        const bool playing = host.isPlaying();
        playButton.setButtonText (juce::String (juce::CharPointer_UTF8 (playing ? "\xe2\x96\xa0 Stop" : "\xe2\x96\xb6 Play")));
        playButton.setColour (juce::TextButton::buttonColourId, playing ? tokens::danger.withAlpha (0.85f) : tokens::play.withAlpha (0.85f));
        loopButton.setColour (juce::TextButton::buttonColourId, host.isLoopingSection() ? tokens::queued.withAlpha (0.85f) : tokens::card);
        const bool hasQueued = host.queuedSection() >= 0;
        jumpNowButton.setEnabled (hasQueued);
        cancelButton.setEnabled (hasQueued);
        timeline.repaint();
        repaint (headerArea.getUnion (dotsArea));
    }

    void addSectionAtPlayhead()
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr) return;
        const int last = (std::max) (0, arr->lengthBars - 1);
        const int bar = juce::jlimit (0, last, (int) std::floor (host.currentBar() + 1.0e-6));
        addSectionAt (bar, addSectionButton.getScreenBounds().getCentre());
    }

    /** One click: pick a name and the section exists, coloured by its name.
        A bar that already starts a section opens that section instead. */
    void addSectionAt (int bar, juce::Point<int> screenPos)
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr) return;
        for (int i = 0; i < (int) arr->sections.size(); ++i)
            if (arr->sections[(size_t) i].startBar == bar) { showSectionMenu (i, screenPos); return; }

        showSectionNamePicker (screenPos, "New section at bar " + juce::String (bar + 1),
                               "Section " + juce::String ((int) arr->sections.size() + 1),
                               [this, bar] (const std::string& name)
        {
            auto* a = host.currentArrangement();
            if (a == nullptr) return;
            // The first section added to an empty song at bar 9 would leave
            // bars 1-8 belonging to nothing, so they become an Intro.
            if (a->sections.empty() && bar > 0)
            {
                ezarr::Section intro;
                intro.name = "Intro";
                intro.startBar = 0;
                intro.colourArgb = colourForSectionName (intro.name, 0);
                a->addSection (intro);
            }
            ezarr::Section s;
            s.name = name;
            s.startBar = bar;
            s.colourArgb = colourForSectionName (name, (int) a->sections.size());
            a->addSection (s);
            host.arrangementEdited();
            songChanged();
        });
    }

    void showSectionNamePicker (juce::Point<int> screenPos, const juce::String& title, const juce::String& customDefault,
                                std::function<void (const std::string&)> onPicked)
    {
        static const char* const names[] = { "Intro", "Verse 1", "Verse 2", "Verse 3", "Pre-Chorus", "Chorus",
                                             "Post-Chorus", "Bridge", "Instrumental", "Interlude", "Breakdown",
                                             "Tag", "Vamp", "Outro", "Ending" };
        juce::PopupMenu m;
        m.addSectionHeader (title);
        // Owner: "type v and verse comes up, so we don't have to scroll a long menu"
        m.addItem (100, "Type a name...  (v = Verse, c = Chorus, b = Bridge)");
        m.addSeparator();
        for (int i = 0; i < (int) juce::numElementsInArray (names); ++i)
            m.addColouredItem (1 + i, names[i], juce::Colour (colourForSectionName (names[i], i)));

        juce::Component::SafePointer<PlaybackView> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                         [safe, onPicked, title, customDefault] (int r)
        {
            if (safe == nullptr || r <= 0) return;
            if (r == 100) { safe->promptName (title, customDefault, onPicked); return; }
            if (r - 1 < (int) juce::numElementsInArray (names)) onPicked (names[r - 1]);
        });
    }

    void promptName (const juce::String& title, const juce::String& current, std::function<void (const std::string&)> onDone)
    {
        auto* w = new juce::AlertWindow (title, "Type the first letters and the name fills in - press Enter to use it.",
                                         juce::MessageBoxIconType::NoIcon);

        static const char* const known[] = {
            "Intro", "Verse", "Verse 1", "Verse 2", "Verse 3", "Verse 4", "Pre-Chorus", "Pre-Chorus 2", "Chorus", "Chorus 2",
            "Chorus 3", "Post-Chorus", "Bridge", "Bridge 2", "Tag", "Turnaround", "Instrumental", "Interlude", "Refrain",
            "Vamp", "Outro", "Ending", "Breakdown", "Build", "Last Chorus", "Rap", "Solo", "Spontaneous", "Acapella",
            "Drums In", "All In", "Worship Freely", "Exhortation", "Key Change"
        };
        juce::StringArray names;
        for (auto* n : known) names.add (n);

        // the field outlives the window: the callback holds it until the window is gone
        auto field = std::make_shared<ezsections::SectionNameField> (names);
        field->setSize (300, 30);
        field->setText (current);
        field->onEnter = [w] { w->exitModalState (1); };
        w->addCustomComponent (field.get());
        w->addButton ("OK", 1);
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([w, field, onDone] (int r)
        {
            const auto name = field->getText().trim().substring (0, 40);
            field->onEnter = nullptr;
            w->removeCustomComponent (0);
            delete w;
            if (r == 1 && name.isNotEmpty()) onDone (name.toStdString());
        }), false);

        juce::Component::SafePointer<ezsections::SectionNameField> focus (field.get());
        juce::MessageManager::callAsync ([focus] { if (focus != nullptr) focus->focusEditor(); });
    }

    void showLaneMenu (int layer, juce::Point<int> screenPos)
    {
        const int song = host.currentSongIndex();
        if (song < 0) return;
        static const char* const presets[] = { "DRUMS", "PERCUSSION", "BASS", "KEYS", "SYNTH", "PADS", "GUITARS",
                                               "ELECTRIC GUITAR", "ACOUSTIC GUITAR", "LEAD VOCAL", "BGVS", "CHOIR",
                                               "STRINGS", "BRASS", "CLICK", "GUIDE" };
        const auto currentName = host.layerName (song, layer);
        juce::PopupMenu m;
        m.addSectionHeader ("Track " + juce::String (layer + 1) + ":  " + currentName);
        for (int i = 0; i < (int) juce::numElementsInArray (presets); ++i)
            m.addItem (1 + i, presets[i], true, currentName == presets[i]);
        m.addSeparator();
        m.addItem (100, "Type a name...");
        m.addItem (101, "Use the file name", host.layerHasCustomName (song, layer));
        m.addSeparator();
        m.addItem (102, host.layerEnabled (song, layer) ? "Mute" : "Unmute");

        juce::Component::SafePointer<PlaybackView> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                         [safe, song, layer, currentName] (int r)
        {
            if (safe == nullptr || r <= 0) return;
            auto& h = safe->host;
            if (r <= (int) juce::numElementsInArray (presets))
                h.setLayerName (song, layer, presets[r - 1]);
            else if (r == 100)
                safe->promptName ("Name track " + juce::String (layer + 1), currentName,
                                  [safe, song, layer] (const std::string& name)
                                  {
                                      if (safe != nullptr)
                                          safe->host.setLayerName (song, layer, juce::String (juce::CharPointer_UTF8 (name.c_str())));
                                  });
            else if (r == 101) h.setLayerName (song, layer, {});
            else if (r == 102) h.toggleLayer (song, layer);
            safe->timeline.repaint();
        });
    }

    void autoSection()
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr || arr->lengthBars <= 0) return;
        juce::PopupMenu m;
        m.addSectionHeader ("Draft sections every...");
        for (int n : { 4, 8, 16 }) m.addItem (n, juce::String (n) + " bars");

        // The song's own cue track, if it has one: "Guide", "Cues", "Click"
        // are the usual names, and they are offered first.
        const int song = host.currentSongIndex();
        if (song >= 0 && host.songHasGuideTrack (song))
        {
            m.addSeparator();
            m.addItem (99, "Listen to the song's guide track");
        }
        std::vector<int> cueLanes, otherLanes;
        for (int l = 0; l < ezdeck::kNumLayers; ++l)
        {
            const auto* s = song >= 0 ? host.layerSamples (song, l) : nullptr;
            if (s == nullptr || s->empty()) continue;
            const auto n = host.layerName (song, l).toLowerCase();
            (n.contains ("guide") || n.contains ("cue") || n.contains ("click") || n.contains ("count") ? cueLanes : otherLanes).push_back (l);
        }
        if (! cueLanes.empty() || ! otherLanes.empty())
        {
            m.addSeparator();
            m.addSectionHeader ("Listen to the cue track...");
            for (int l : cueLanes)   m.addItem (100 + l, host.layerName (song, l));
            if (! cueLanes.empty() && ! otherLanes.empty()) m.addSeparator();
            for (int l : otherLanes) m.addItem (100 + l, host.layerName (song, l));
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (autoSectionButton), [this, song] (int n)
        {
            if (n <= 0) return;
            if (n == 99)  { host.listenForSections (song, -1); return; }
            if (n >= 100) { host.listenForSections (song, n - 100); return; }
            if (auto* a = host.currentArrangement())
            {
                a->autoSection (n, ezarr::worshipSectionNames());
                for (int i = 0; i < (int) a->sections.size(); ++i)
                    a->sections[(size_t) i].colourArgb = colourForSectionName (a->sections[(size_t) i].name, i);
                host.arrangementEdited();
                songChanged();
            }
        });
    }

    void showSectionMenu (int s, juce::Point<int> screenPos)
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr || s < 0 || s >= (int) arr->sections.size()) return;
        const auto& sec = arr->sections[(size_t) s];

        juce::PopupMenu m;
        m.addSectionHeader (juce::String (juce::CharPointer_UTF8 (sec.name.c_str())));
        m.addItem (1, "Jump here");
        m.addItem (2, "Rename...");
        m.addSeparator();
        m.addItem (3, "Loop when reached", true, sec.loopOnEntry);
        m.addItem (4, "Pause after", true, sec.pauseAfter);
        m.addItem (5, "Skip in sequence", true, sec.skip);
        m.addItem (6, "Optional (only when called)", true, sec.optional);
        m.addSeparator();
        juce::PopupMenu colours;
        for (int i = 0; i < 8; ++i) colours.addColouredItem (100 + i, "Colour " + juce::String (i + 1), juce::Colour (ezarr::defaultSectionColour (i)));
        m.addSubMenu ("Colour", colours);
        if (host.cueBankLoaded())
        {
            juce::PopupMenu cue;
            cue.addItem (200, "Spoken from the name", true, sec.cue.empty());
            cue.addItem (201, "Silent", true, ezguide::cueIsOff (sec.cue));
            cue.addSeparator();
            addCueChoices (cue, 300, juce::String (juce::CharPointer_UTF8 (sec.cue.c_str())));
            m.addSubMenu ("Cue", cue);
        }
        m.addSeparator();
        m.addItem (9, "Delete section", s > 0);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                         [this, s, screenPos] (int r)
        {
            auto* a = host.currentArrangement();
            if (r == 0 || a == nullptr || s >= (int) a->sections.size()) return;
            auto& sec = a->sections[(size_t) s];
            switch (r)
            {
                case 1: host.jumpToSection (s); return;
                case 2: renameSection (s, screenPos); return;
                case 3: sec.loopOnEntry = ! sec.loopOnEntry; break;
                case 4: sec.pauseAfter  = ! sec.pauseAfter;  break;
                case 5: sec.skip        = ! sec.skip;        break;
                case 6: sec.optional    = ! sec.optional;    break;
                case 9: a->removeSection (s); break;
                case 200: sec.cue.clear(); break;
                case 201: sec.cue = "-"; break;
                default:
                    if (r >= 100 && r < 108) sec.colourArgb = ezarr::defaultSectionColour (r - 100);
                    else if (r >= 300)
                    {
                        const auto names = host.cueNames();
                        if (r - 300 < names.size())
                        {
                            sec.cue = names[r - 300].fromLastOccurrenceOf ("/", false, false).toStdString();
                            host.previewCue (juce::String (sec.cue));
                        }
                    }
                    break;
            }
            host.arrangementEdited();
            songChanged();
        });
    }

    /** The bank's cues as submenus per group, ids from base in cueNames() order. */
    void addCueChoices (juce::PopupMenu& into, int base, const juce::String& current)
    {
        const auto names = host.cueNames();
        juce::String group;
        juce::PopupMenu sub;
        auto flush = [&] { if (group.isNotEmpty()) into.addSubMenu (group, sub); sub = juce::PopupMenu(); };
        for (int i = 0; i < names.size(); ++i)
        {
            const auto g = names[i].upToLastOccurrenceOf ("/", false, false);
            const auto stem = names[i].fromLastOccurrenceOf ("/", false, false);
            if (g != group) { flush(); group = g; }
            sub.addItem (base + i, stem.replaceCharacter ('-', ' '), true, stem == current);
        }
        flush();
    }

    void cuesMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Cues on for this song", true, host.guideCues());
        m.addSeparator();
        m.addSectionHeader ("Say the section name...");
        for (int b : { 1, 2, 4 }) m.addItem (10 + b, juce::String (b) + (b == 1 ? " bar early" : " bars early"), true, host.cueLeadBars() == b);
        m.addSeparator();
        m.addItem (20, "Count \"1, 2, 3, 4\" into each section", true, host.cueCounts());
        m.addSeparator();
        juce::PopupMenu say;
        addCueChoices (say, 300, {});
        m.addSubMenu ("Say now...", say);
        juce::Component::SafePointer<PlaybackView> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (cuesButton), [safe] (int r)
        {
            if (safe == nullptr || r <= 0) return;
            auto& h = safe->host;
            if (r == 1) h.setGuideCues (! h.guideCues());
            else if (r >= 11 && r <= 14) h.setCueLeadBars (r - 10);
            else if (r == 20) h.setCueCounts (! h.cueCounts());
            else if (r >= 300)
            {
                const auto names = h.cueNames();
                if (r - 300 < names.size()) h.previewCue (names[r - 300].fromLastOccurrenceOf ("/", false, false));
                return;
            }
            safe->songChanged();
        });
    }

    void renameSection (int s, juce::Point<int> screenPos)
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr || s < 0 || s >= (int) arr->sections.size()) return;
        const auto oldName = arr->sections[(size_t) s].name;
        showSectionNamePicker (screenPos, "Rename section", juce::String (juce::CharPointer_UTF8 (oldName.c_str())),
                               [this, s, oldName] (const std::string& name)
        {
            auto* a = host.currentArrangement();
            if (a == nullptr || s >= (int) a->sections.size()) return;
            auto& sec = a->sections[(size_t) s];
            // Keep a colour the performer chose by hand; follow the new name
            // only while the colour is still the automatic one.
            if (sec.colourArgb == 0 || sec.colourArgb == colourForSectionName (oldName, s))
                sec.colourArgb = colourForSectionName (name, s);
            sec.name = name;
            host.arrangementEdited();
            songChanged();
        });
    }

    PlaybackHost& host;
    Timeline timeline;
    SetlistPanel setlist;
    juce::Rectangle<int> headerArea, dotsArea;
    juce::TextButton prevSongButton, prevSectionButton, playButton, countInButton, nextSectionButton, nextSongButton,
                     loopButton, jumpNowButton, cancelButton, autoSectionButton, addSectionButton, clickButton, cuesButton;
    juce::ComboBox jumpModeBox, countInBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackView)
};

} // namespace ezplayback
