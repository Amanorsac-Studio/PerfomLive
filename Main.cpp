#include <JuceHeader.h>
#include <BinaryData.h>     // PerformLiveArt: the embedded header wordmark
#include "ActionRegistry.h"
#include "DeckCard.h"
#include "Guide.h"
#include "TouchSupport.h"   // press-and-hold = right-click, for iPad and touchscreens
#include "InstrumentHost.h"
#include "CueDetect.h"
#include "CreatorsTab.h"
#include "ChannelStrip/PerformEditor.h"   // PERFORM LIVE, compiled in
#include "EzDSP.h"
#include "Importer.h"
#include "KeyBindingMap.h"
#include "Library.h"
#include "Metronome.h"
#include "MidiActionRouter.h"
#include "Mixer.h"
#include "OneShotVoice.h"
#include "ProjectFile.h"
#include "Session.h"
#include "SignatureManager.h"
#include "Arrangement.h"
#include "UiArt.h"
#include "ProductPaths.h"   // the only two folders the app writes to (Build Standard B48)
#include "Beta.h"           // PERFORMLIVE BETA (Testing): label and fixed end date
#include "CreatorsTab.h"    // the STORE page for the beta, and the end-of-beta window
#include "ProjectFile.h"

// PX-B: these two arrays are indexed by loops bounded by ezdeck::kNumLayers.
// When the engine grew from 4 layers to 8 and these did not, the result was
// not a crash but a HANG on startup -- the loop read past the end of the
// snapshot into whatever followed it and tried to treat that as a
// juce::String. Silent, and expensive to find. The schema is deliberately
// not defined in terms of the engine constant (an on-disk format must not
// silently change meaning when an engine value moves), so this assert is
// what keeps the two in step instead.
static_assert (ezproject::DeckSnapshot::kDeckLayers == ezdeck::kNumLayers,
               "DeckSnapshot::layers must have one entry per engine layer");
static_assert (ezproject::SettingsSnapshot::kLiveColumns == ezdeck::kNumLayers,
               "legacy live-input fields: one per column");
static_assert (ezproject::SettingsSnapshot::kLiveTracks == ezdeck::kNumLiveTracks,
               "SettingsSnapshot's live-track arrays must have one entry per live track");
static_assert (ezproject::SettingsSnapshot::kStrips == ezdeck::kNumLayers + ezdeck::kNumLiveTracks,
               "one channel strip per deck and per live track");
static_assert ((int) ezdeck::MixerChannel::Live1 == ezdeck::kNumLayers,
               "the live-track mixer channels follow the decks, so strip index == mixer channel index");
static_assert (ezproject::ProjectSnapshot::kMixerChannels == ezdeck::kNumMixerChannels,
               "ProjectSnapshot::mixerChannels must have one entry per mixer channel");
static_assert (ezproject::SceneSnapshot::kSceneTabs == ezdeck::kNumLayers,
               "SceneSnapshot::tabEnabled must have one entry per engine layer");
static_assert (ezproject::ProjectSnapshot::kMixerChannels == ezdeck::kNumMixerChannels,
               "ProjectSnapshot::mixerChannels must have one entry per mixer channel");
#include "PlaybackView.h"
#include "StemImport.h"     // the window that opens when a set of stems lands on a row
#include "SpeechCues.h"     // hearing the words on a guide track
#include "CueReview.h"      // checking those sections before they're applied
#include "StoreShowcase.h"  // the STORE page's preview catalogue
#include "BrowserTab.h"
#include "WarpIntegration.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <vector>

//==============================================================================
//  PerformLive font stack (owner direction): Space Grotesk for headings/UI
//  chrome, Inter for body text, JetBrains Mono for numeric/time readouts.
//  Shipped as OFL-licensed latin-subset TTFs in ./fonts (next to the exe /
//  working dir / C:/EzPlay -- the same 3-candidate search convention every
//  other asset in this file uses), loaded once and cached. Every accessor
//  returns nullptr when its file is missing, and every consumer falls back
//  to the system font in that case -- a missing fonts folder degrades the
//  look, never the app.
//
//  Routing (see PerformLookAndFeel near the app class at the bottom):
//  - default sans-serif placeholder  -> Inter (bold requests get the real
//    Inter Bold file, not a synthesised bold)
//  - default monospace placeholder   -> JetBrains Mono (this is the name
//    every numeric readout in this file already requests, so they all
//    switch over with no call-site changes)
//  - headings ask for Space Grotesk EXPLICITLY via headingFont() below,
//    which binds the typeface directly (a named lookup would go to the OS,
//    which doesn't have these fonts installed).
//==============================================================================
namespace performfonts
{
    inline juce::Typeface::Ptr loadTypeface (const char* filename)
    {
        auto tryLoad = [] (const juce::File& f) -> juce::Typeface::Ptr
        {
            if (! f.existsAsFile()) return nullptr;
            juce::MemoryBlock data;
            if (! f.loadFileAsData (data)) return nullptr;
            return juce::Typeface::createSystemTypefaceFor (data.getData(), data.getSize());
        };
        const juce::File candidates[] = {
            productpaths::bundledResources().getChildFile ("fonts").getChildFile (filename),
            juce::File::getCurrentWorkingDirectory().getChildFile ("fonts").getChildFile (filename),
        };
        for (const auto& c : candidates)
            if (auto t = tryLoad (c)) return t;
        return nullptr;
    }

    // Function-local statics: loaded on first use, cached for the app's life.
    inline juce::Typeface::Ptr inter()        { static auto t = loadTypeface ("Inter-Regular.ttf");        return t; }
    inline juce::Typeface::Ptr interBold()    { static auto t = loadTypeface ("Inter-Bold.ttf");           return t; }
    inline juce::Typeface::Ptr grotesk()      { static auto t = loadTypeface ("SpaceGrotesk-Medium.ttf");  return t; }
    inline juce::Typeface::Ptr groteskBold()  { static auto t = loadTypeface ("SpaceGrotesk-Bold.ttf");    return t; }
    inline juce::Typeface::Ptr mono()         { static auto t = loadTypeface ("JetBrainsMono-Regular.ttf"); return t; }
    inline juce::Typeface::Ptr monoBold()     { static auto t = loadTypeface ("JetBrainsMono-Bold.ttf");   return t; }

    // Space Grotesk heading -- binds the typeface directly (no OS lookup);
    // falls back to the default sans (which PerformLookAndFeel maps to
    // Inter) if the font file is missing.
    inline juce::Font headingFont (float size, bool bold = true)
    {
        if (auto tf = bold ? groteskBold() : grotesk())
            return juce::Font (juce::FontOptions (tf).withHeight (size));
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }
}

//==============================================================================
//  Owner request ("the whole app should accept drag and drop from Windows"):
//  one shared answer to "is this path an audio file we can load?", used by
//  every FileDragAndDropTarget below -- the same extension list every
//  FileChooser in this file already offers.
//==============================================================================
inline bool isSupportedAudioPath (const juce::String& path)
{
    static const char* kExts[] = { "*.wav", "*.aif", "*.aiff", "*.flac", "*.mp3", "*.ogg" };
    for (auto* ext : kExts)
        if (path.matchesWildcard (ext, true)) return true;
    return false;
}

inline bool anySupportedAudioPath (const juce::StringArray& paths)
{
    for (const auto& p : paths)
        if (isSupportedAudioPath (p)) return true;
    return false;
}

//==============================================================================
//  Owner question ("some files have tempo embedded in them from export --
//  are you able to see that?"): yes, when the format carries it. DAW-exported
//  WAVs often include an ACID chunk with the authored tempo (JUCE surfaces it
//  in AudioFormatReader::metadataValues), and some tools write plain
//  "bpm"/ID3-style tags. An embedded tempo is the AUTHOR's number, so every
//  load/import path prefers it over our own detection (which stays as the
//  fallback). Returns 0.0 when nothing usable is embedded.
//==============================================================================
inline double embeddedTempoFrom (const juce::AudioFormatReader& reader)
{
    for (const char* key : { "acid tempo", "bpm", "BPM", "TBPM", "tempo" })
    {
        const auto v = reader.metadataValues.getValue (key, {});
        if (v.isNotEmpty())
        {
            const double bpm = v.getDoubleValue();
            if (bpm > 20.0 && bpm < 999.0) return bpm;
        }
    }
    return 0.0;
}

//==============================================================================
//  Owner #8: "I want all the menus to have the colours (actual colour) in
//  boxes on top, then the menu items below -- so no Set Colour." One shared
//  PopupMenu custom row: the app's colour presets as tappable boxes plus a
//  slashed "no colour" box. Embedded at the top of every context menu that
//  colours something (deck layers, rows, scenes, signatures, pads/FX);
//  picking a box applies immediately and dismisses the menu.
//==============================================================================
class ColourSwatchRow : public juce::PopupMenu::CustomComponent
{
public:
    ColourSwatchRow (bool hasColourNow,
                     std::function<void (juce::uint32)> onPickFn,
                     std::function<void()> onClearFn)
        : juce::PopupMenu::CustomComponent (false),
          showClear (hasColourNow), onPick (std::move (onPickFn)), onClear (std::move (onClearFn)) {}

    static constexpr int kNumSwatches = 8;
    static const juce::uint32* swatches()
    {
        static const juce::uint32 s[kNumSwatches] = {
            0xffa855f7, 0xff4d7cff, 0xffff3b5c, 0xffff7a1a,   // purple blue red orange
            0xff2ee86a, 0xffffc933, 0xffff2d95, 0xff2de8c8,   // green yellow pink teal
        };
        return s;
    }

    void getIdealSize (int& w, int& h) override { w = 6 + (kNumSwatches + 1) * 30 + 6; h = 38; }

    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < kNumSwatches; ++i)
        {
            auto r = boxAt (i).toFloat();
            g.setColour (juce::Colour (swatches()[i]));
            g.fillRoundedRectangle (r, 5.0f);
            if (i == hoverIndex)
            {
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.drawRoundedRectangle (r, 5.0f, 2.0f);
            }
        }
        // the "no colour" box: dark card with a diagonal slash, dimmed when
        // there's nothing to clear
        auto r = boxAt (kNumSwatches).toFloat();
        g.setColour (juce::Colour (0xff151527).withAlpha (showClear ? 1.0f : 0.5f));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (juce::Colour (0xff6f7099).withAlpha (showClear ? 1.0f : 0.5f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        g.drawLine (r.getX() + 6.0f, r.getBottom() - 6.0f, r.getRight() - 6.0f, r.getY() + 6.0f, 1.5f);
        if (hoverIndex == kNumSwatches && showClear)
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawRoundedRectangle (r, 5.0f, 2.0f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        int newHover = -1;
        for (int i = 0; i <= kNumSwatches; ++i)
            if (boxAt (i).contains (e.getPosition())) { newHover = i; break; }
        if (newHover != hoverIndex) { hoverIndex = newHover; repaint(); }
    }
    void mouseExit (const juce::MouseEvent&) override { hoverIndex = -1; repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < kNumSwatches; ++i)
            if (boxAt (i).contains (e.getPosition()))
            {
                if (onPick) onPick (swatches()[i]);
                triggerMenuItem();
                return;
            }
        if (showClear && boxAt (kNumSwatches).contains (e.getPosition()))
        {
            if (onClear) onClear();
            triggerMenuItem();
        }
    }

private:
    juce::Rectangle<int> boxAt (int i) const { return { 6 + i * 30, 6, 26, 26 }; }

    bool showClear;
    int hoverIndex { -1 };
    std::function<void (juce::uint32)> onPick;
    std::function<void()> onClear;
};

//==============================================================================
//  The app-wide default LookAndFeel. Font resolution for the placeholder
//  names ("<Sans-Serif>"/"<Monospaced>") is GLOBAL in JUCE -- every Font,
//  including ones set directly on a Graphics, resolves through the DEFAULT
//  LookAndFeel's getTypefaceForFont(), regardless of any per-component
//  LookAndFeel -- so this one override retypes the entire app: every
//  existing default-sans call site becomes Inter and every existing
//  default-monospace call site (all the numeric readouts) becomes
//  JetBrains Mono, with the real Bold files used for bold requests
//  (returning just one typeface for both weights would silently lose bold).
//  Installed/removed in EzPlayApplication::initialise()/shutdown().
//==============================================================================
class PerformLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Owner bug report ("that part of the app feels very foreign and not
    // consistent with the app design... same as the settings window"):
    // LookAndFeel_V4's stock grey colour scheme was leaking into every
    // default-styled widget -- the header's volume slider and its "100%"
    // box, combo dropdown lists, popup menus, alert windows, scrollbars,
    // and the whole Settings panel. One scheme swap retints all of them
    // with the app's own palette (values == performlive::, which is
    // declared later in the file -- same raw-hex convention as every
    // class defined before that namespace).
    PerformLookAndFeel()
    {
        setColourScheme ({ juce::Colour (0xff0c0c17),     // windowBackground  == kShellBg
                           juce::Colour (0xff151527),     // widgetBackground  == kCard
                           juce::Colour (0xff151527),     // menuBackground    == kCard
                           juce::Colour (0xff2b2b4d),     // outline           == kBorder
                           juce::Colour (0xfff2f0ff),     // defaultText       == kTextBright
                           juce::Colour (0xff3a2f6b),     // defaultFill       == kIndigoDim
                           juce::Colour (0xfff2f0ff),     // highlightedText   == kTextBright
                           juce::Colour (0xff7c5cff),     // highlightedFill   == kIndigo
                           juce::Colour (0xfff2f0ff) });  // menuText          == kTextBright
        applyWidgetColours();
    }

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& font) override
    {
        const auto name = font.getTypefaceName();
        if (name == juce::Font::getDefaultSansSerifFontName())
            if (auto tf = font.isBold() ? performfonts::interBold() : performfonts::inter())
                return tf;
        if (name == juce::Font::getDefaultMonospacedFontName())
            if (auto tf = font.isBold() ? performfonts::monoBold() : performfonts::mono())
                return tf;
        return juce::LookAndFeel_V4::getTypefaceForFont (font);
    }

    //==========================================================================
    //  Owner: "make the sub-menus, pop-up menus and the download tab all feel
    //  consistent with the app and easy to use." Everything below is the
    //  stock JUCE widget redrawn in the app's own palette, with rows a
    //  finger can hit on stage: 32 px menu rows, rounded cards, the indigo
    //  highlight, and the same 1 px border every panel in the app uses.
    //==========================================================================
    static constexpr juce::uint32 kLafShell   = 0xff0c0c17, kLafCard = 0xff151527, kLafBorder = 0xff2b2b4d,
                                  kLafIndigo  = 0xff7c5cff, kLafIndigoDim = 0xff3a2f6b,
                                  kLafBright  = 0xfff2f0ff, kLafDim = 0xffa3a6cc, kLafFaint = 0xff6f7099,
                                  kLafDanger  = 0xffff3b5c;

    // ---- popup menus -----------------------------------------------------------
    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (14.0f)); }

    int getPopupMenuBorderSizeWithOptions (const juce::PopupMenu::Options&) override { return 6; }

    void getIdealPopupMenuItemSizeWithOptions (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                               int& idealWidth, int& idealHeight, const juce::PopupMenu::Options&) override
    {
        if (isSeparator) { idealWidth = 50; idealHeight = 9; return; }
        auto font = getPopupMenuFont();
        if (standardMenuItemHeight > 0 && font.getHeight() > (float) standardMenuItemHeight / 1.3f)
            font.setHeight ((float) standardMenuItemHeight / 1.3f);
        idealHeight = juce::jmax (32, standardMenuItemHeight);
        idealWidth  = juce::GlyphArrangement::getStringWidthInt (font, text) + 58;
    }

    void drawPopupMenuBackgroundWithOptions (juce::Graphics& g, int width, int height, const juce::PopupMenu::Options&) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
        g.setColour (juce::Colour (kLafCard));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (juce::Colour (kLafBorder));
        g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
    }

    void drawPopupMenuSectionHeaderWithOptions (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName,
                                                const juce::PopupMenu::Options&) override
    {
        g.setColour (juce::Colour (kLafFaint));
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)).withExtraKerningFactor (0.08f));
        g.drawFittedText (sectionName.toUpperCase(), area.reduced (14, 0).withTrimmedTop (4), juce::Justification::bottomLeft, 1);
    }

    void drawPopupMenuItemWithOptions (juce::Graphics& g, const juce::Rectangle<int>& area, bool isHighlighted,
                                       const juce::PopupMenu::Item& item, const juce::PopupMenu::Options&) override
    {
        if (item.isSeparator)
        {
            g.setColour (juce::Colour (kLafBorder));
            g.fillRect (area.reduced (12, 0).withHeight (1).withY (area.getCentreY()));
            return;
        }
        if (item.isSectionHeader)
        {
            drawPopupMenuSectionHeaderWithOptions (g, area, item.text, {});
            return;
        }

        auto r = area.reduced (6, 1);
        const bool enabled = item.isEnabled;
        if (isHighlighted && enabled)
        {
            g.setColour (juce::Colour (kLafIndigo));
            g.fillRoundedRectangle (r.toFloat(), 7.0f);
        }

        auto textColour = item.colour != juce::Colour() ? item.colour
                        : juce::Colour (enabled ? (isHighlighted ? kLafBright : kLafBright) : kLafFaint);
        if (isHighlighted && enabled && item.colour == juce::Colour()) textColour = juce::Colour (kLafBright);
        if (isHighlighted && enabled && item.colour != juce::Colour()) textColour = item.colour.brighter (0.6f);

        auto inner = r.reduced (10, 0);
        // tick / colour swatch on the left
        auto lead = inner.removeFromLeft (18);
        if (item.isTicked)
        {
            juce::Path tick;
            const auto t = lead.toFloat().withSizeKeepingCentre (11.0f, 9.0f);
            tick.startNewSubPath (t.getX(), t.getCentreY());
            tick.lineTo (t.getX() + t.getWidth() * 0.38f, t.getBottom());
            tick.lineTo (t.getRight(), t.getY());
            g.setColour (isHighlighted ? juce::Colour (kLafBright) : juce::Colour (kLafIndigo).brighter (0.3f));
            g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (item.colour != juce::Colour())
        {
            g.setColour (item.colour);
            g.fillEllipse (lead.toFloat().withSizeKeepingCentre (9.0f, 9.0f));
        }
        inner.removeFromLeft (6);

        if (item.subMenu != nullptr)
        {
            auto arrow = inner.removeFromRight (14).toFloat().withSizeKeepingCentre (6.0f, 10.0f);
            juce::Path p;
            p.startNewSubPath (arrow.getX(), arrow.getY());
            p.lineTo (arrow.getRight(), arrow.getCentreY());
            p.lineTo (arrow.getX(), arrow.getBottom());
            g.setColour (juce::Colour (enabled ? (isHighlighted ? kLafBright : kLafDim) : kLafFaint));
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (item.shortcutKeyDescription.isNotEmpty())
        {
            g.setColour (juce::Colour (isHighlighted && enabled ? kLafBright : kLafFaint).withAlpha (0.9f));
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
            g.drawText (item.shortcutKeyDescription, inner.removeFromRight (70), juce::Justification::centredRight, false);
        }

        g.setColour (textColour);
        g.setFont (getPopupMenuFont());
        g.drawFittedText (item.text, inner, juce::Justification::centredLeft, 1);
    }

    // ---- alert windows (Rename..., New section, Recover...) -------------------
    juce::Font getAlertWindowTitleFont() override   { return juce::Font (juce::FontOptions (17.0f, juce::Font::bold)); }
    juce::Font getAlertWindowMessageFont() override { return juce::Font (juce::FontOptions (13.5f)); }
    juce::Font getAlertWindowFont() override        { return juce::Font (juce::FontOptions (13.0f)); }

    void drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea,
                       juce::TextLayout& textLayout) override
    {
        const auto r = alert.getLocalBounds().toFloat();
        g.setColour (juce::Colour (kLafCard));
        g.fillRoundedRectangle (r, 12.0f);
        g.setColour (juce::Colour (kLafIndigo).withAlpha (0.9f));
        g.fillRoundedRectangle (r.withWidth (5.0f).reduced (0.0f, 14.0f), 2.5f);
        g.setColour (juce::Colour (kLafBorder));
        g.drawRoundedRectangle (alert.getLocalBounds().toFloat().reduced (0.5f), 12.0f, 1.0f);

        // JUCE's AlertWindow already puts the title (bold) and the message into
        // textLayout, laid out for textArea. Drawing the title again here is
        // what printed it twice on top of itself ("jumbled words").
        textLayout.draw (g, textArea.toFloat());
    }

    int getAlertWindowButtonHeight() override { return 34; }

    // ---- buttons in dialogs / lists / the WEB bar -----------------------------
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& backgroundColour,
                               bool isHighlighted, bool isDown) override
    {
        const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = b.getToggleState();
        auto fill = backgroundColour;
        if (fill == juce::Colour (kLafCard) || fill.getBrightness() < 0.2f) fill = juce::Colour (on ? kLafIndigo : kLafCard);
        if (isDown) fill = fill.brighter (0.25f);
        else if (isHighlighted) fill = fill.brighter (0.10f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (juce::Colour (on ? kLafIndigo : kLafBorder).brighter (isHighlighted ? 0.3f : 0.0f));
        g.drawRoundedRectangle (r, 8.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions (juce::jmin (14.0f, (float) buttonHeight * 0.45f), juce::Font::bold));
    }

    // ---- tooltips ------------------------------------------------------------------
    static juce::TextLayout layoutTooltipText (const juce::String& text, juce::Colour colour)
    {
        juce::AttributedString s;
        s.setJustification (juce::Justification::centredLeft);
        s.append (text, juce::Font (juce::FontOptions (13.0f)), colour);
        juce::TextLayout tl;
        tl.createLayout (s, 340.0f);
        return tl;
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override
    {
        const juce::TextLayout tl = layoutTooltipText (tipText, juce::Colour (kLafBright));
        const int w = (int) (tl.getWidth() + 22.0f), h = (int) (tl.getHeight() + 14.0f);
        return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                     screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
                   .constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
        g.setColour (juce::Colour (kLafShell).withAlpha (0.97f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (juce::Colour (kLafBorder));
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
        layoutTooltipText (text, juce::Colour (kLafBright)).draw (g, r.reduced (11.0f, 7.0f));
    }

    // ---- scrollbars / progress ------------------------------------------------------
    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool isVertical,
                        int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown) override
    {
        juce::Rectangle<int> thumb = isVertical ? juce::Rectangle<int> (x + 2, thumbStart, width - 4, thumbSize)
                                                : juce::Rectangle<int> (thumbStart, y + 2, thumbSize, height - 4);
        g.setColour (juce::Colour (kLafBorder).brighter (isMouseDown ? 0.5f : isMouseOver ? 0.25f : 0.0f));
        g.fillRoundedRectangle (thumb.toFloat(), 3.0f);
    }

    int getDefaultScrollbarWidth() override { return 10; }

    void drawProgressBar (juce::Graphics& g, juce::ProgressBar& bar, int width, int height, double progress,
                          const juce::String& textToShow) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
        g.setColour (juce::Colour (kLafShell));
        g.fillRoundedRectangle (r, 5.0f);
        if (progress >= 0.0 && progress <= 1.0)
        {
            g.setColour (juce::Colour (kLafIndigo));
            g.fillRoundedRectangle (r.withWidth ((float) width * (float) progress), 5.0f);
        }
        else
        {
            // indeterminate: a travelling indigo segment
            const float t = (float) ((juce::Time::getMillisecondCounter() / 12) % (uint32_t) juce::jmax (1, width * 2)) / (float) juce::jmax (1, width * 2);
            const float segW = (float) width * 0.25f;
            const float pos = t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
            g.setColour (juce::Colour (kLafIndigo));
            g.fillRoundedRectangle (r.withX (pos * ((float) width - segW)).withWidth (segW), 5.0f);
        }
        if (textToShow.isNotEmpty())
        {
            g.setColour (juce::Colour (kLafBright));
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (textToShow, r.toNearestInt(), juce::Justification::centred, false);
        }
        juce::ignoreUnused (bar);
    }

    // ---- combo boxes / text editors / lists: colours the stock drawers pick up ----
    void applyWidgetColours()
    {
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (kLafCard));
        setColour (juce::ComboBox::outlineColourId,    juce::Colour (kLafBorder));
        setColour (juce::ComboBox::textColourId,       juce::Colour (kLafBright));
        setColour (juce::ComboBox::arrowColourId,      juce::Colour (kLafDim));
        setColour (juce::ComboBox::focusedOutlineColourId, juce::Colour (kLafIndigo));
        setColour (juce::TextEditor::backgroundColourId,     juce::Colour (kLafShell));
        setColour (juce::TextEditor::outlineColourId,        juce::Colour (kLafBorder));
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (kLafIndigo));
        setColour (juce::TextEditor::textColourId,           juce::Colour (kLafBright));
        setColour (juce::TextEditor::highlightColourId,      juce::Colour (kLafIndigoDim));
        setColour (juce::TextEditor::highlightedTextColourId, juce::Colour (kLafBright));
        setColour (juce::CaretComponent::caretColourId,      juce::Colour (kLafIndigo));
        setColour (juce::TextButton::buttonColourId,   juce::Colour (kLafCard));
        setColour (juce::TextButton::buttonOnColourId, juce::Colour (kLafIndigo));
        setColour (juce::TextButton::textColourOffId,  juce::Colour (kLafBright));
        setColour (juce::TextButton::textColourOnId,   juce::Colour (kLafBright));
        setColour (juce::AlertWindow::backgroundColourId, juce::Colour (kLafCard));
        setColour (juce::AlertWindow::textColourId,       juce::Colour (kLafBright));
        setColour (juce::AlertWindow::outlineColourId,    juce::Colour (kLafBorder));
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (kLafShell));
        setColour (juce::TooltipWindow::textColourId,       juce::Colour (kLafBright));
        setColour (juce::TooltipWindow::outlineColourId,    juce::Colour (kLafBorder));
        setColour (juce::ListBox::backgroundColourId,       juce::Colour (kLafShell));
        setColour (juce::TableHeaderComponent::backgroundColourId, juce::Colour (kLafCard));
        setColour (juce::TableHeaderComponent::textColourId,       juce::Colour (kLafDim));
        setColour (juce::TableHeaderComponent::outlineColourId,    juce::Colour (kLafBorder));
        setColour (juce::TableHeaderComponent::highlightColourId,  juce::Colour (kLafIndigoDim));
        setColour (juce::ProgressBar::backgroundColourId, juce::Colour (kLafShell));
        setColour (juce::ProgressBar::foregroundColourId, juce::Colour (kLafIndigo));
        setColour (juce::ScrollBar::thumbColourId,        juce::Colour (kLafBorder));
        setColour (juce::PopupMenu::backgroundColourId,   juce::Colour (kLafCard));
        setColour (juce::PopupMenu::textColourId,         juce::Colour (kLafBright));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (kLafIndigo));
        setColour (juce::PopupMenu::highlightedTextColourId,       juce::Colour (kLafBright));
        setColour (juce::PopupMenu::headerTextColourId,   juce::Colour (kLafFaint));
        setColour (juce::Label::textColourId,             juce::Colour (kLafBright));
        setColour (juce::ToggleButton::textColourId,      juce::Colour (kLafBright));
        setColour (juce::ToggleButton::tickColourId,      juce::Colour (kLafIndigo));
        setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (kLafFaint));
        setColour (juce::DocumentWindow::backgroundColourId, juce::Colour (kLafShell));
        setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (kLafShell));
    }
};

//==============================================================================
//  EzPlay — milestone 3: bar-quantized deck switching.
//
//  Two decks (A and B), each 4 phase-locked layers (see Deck.h), managed by
//  one ezdeck::Session (see Session.h). Only one deck is audible at a time.
//  Triggering the other deck doesn't cut over immediately — the switch is
//  queued for the next bar boundary, computed from masterTempo below, and
//  the new deck starts at playhead 0 so it's phase-aligned to bar zero.
//
//  Clock discipline (unchanged since milestone 1, still the whole point):
//  playback is driven entirely by the pulled getNextAudioBlock callback.
//  Session::render does all its bar-boundary bookkeeping using an absolute
//  sample count, so it behaves identically no matter how the host chunks
//  callbacks — the property proven in switchtest.cpp. The Timer below only
//  repaints the UI; it never drives audio.
//==============================================================================

//==============================================================================
//  UI_SPEC_PERFORM.md §2.4 -- the deck grid's 72px trigger column.
//  DeckTriggerButton (the plain right-click-hook TextButton this class
//  originally sat alongside) is gone -- UI_SPEC_PERFORM.md step 5 replaced
//  its other user, VoiceBankPanel's Pads/FX slots, with the richer
//  VoiceSlotCell below, leaving DeckTriggerButton fully unreferenced.
//
//  Hardcodes the same hex values as performlive:: (this file's palette is
//  declared after SessionComponent's own forward point; matching DeckCard.h's
//  own convention of self-contained literal colours rather than depending on
//  include order).
//
//  No internal Timer -- state/pulse phase are pushed in by the owner from
//  the existing 15 Hz UI timer, exactly like DeckCard's setBeatPhase().
//==============================================================================
class DeckTriggerCell : public juce::Component, public juce::SettableTooltipClient
{
public:
    // SPEC_PERFORM_V2 GROUP A: added `armed` -- this row IS the selected
    // deck but the transport is stopped (full colour, outlined, static).
    // Distinct from `playing` (selected AND transportRunning -- brighter,
    // no separate wash difference here since the trigger cell's own
    // "playing" wash already reads as the brighter state) and from
    // `queued` (a DIFFERENT row pending a bar-quantized swap while THIS
    // row is still the one currently playing).
    enum class State { idle, queued, armed, playing };

    // Explicit, even though it does nothing extra: JUCE_DECLARE_NON_COPYABLE
    // below deletes the copy constructor, which counts as a user-declared
    // constructor and suppresses the implicit default one too -- matching
    // why DeckCard.h's own constructor isn't just `= default`.
    DeckTriggerCell() = default;

    std::function<void()> onTrigger;   // left-click / tap -- switches to this deck
    std::function<void()> onMenu;      // right-click -- same context menu as before
    // Owner #9: "stem or loop mode using simbols that can be toggled" --
    // tapping the mode symbol pill (bottom of the cell) fires this instead
    // of onTrigger. Owner wires it to the same mode flip the row menu's
    // "Switch to stem/loop mode" item performs, same canMutateDeckState gate.
    std::function<void()> onModeToggle;

    void setState (State s)
    {
        if (state == s) return;
        state = s;
        repaint();
    }

    /** 0..1 within the 2 Hz queued-pulse cycle; ignored unless state == queued. */
    void setPulsePhase (double phase01)
    {
        pulsePhase = juce::jlimit (0.0, 1.0, phase01);
        if (state == State::queued) repaint();
    }

    // Owner #9: "the play A1 big (or Name) then 4/4 below then below is
    // stem or loop mode using simbols" -- the old single "4/4 A1" label is
    // split into a big slot name and a separate signature line, and the
    // "LOOP"/"STEM->..." text is replaced by a drawn symbol (setModeIsStem).
    void setSlotName  (const juce::String& n)  { slotNameText = n; repaint(); }
    void setSigText   (const juce::String& s)  { sigText = s; repaint(); }
    void setTempoText (const juce::String& t)  { tempoText = t; repaint(); }
    void setModeIsStem (bool isStem)           { if (modeIsStem != isStem) { modeIsStem = isStem; repaint(); } }

    // Phase 1.1 P1 "Color Row": a purely cosmetic left-edge accent strip,
    // deliberately additive rather than replacing the existing idle/queued/
    // playing state colouring above -- a custom row colour and "this row is
    // about to switch"/"this row is playing" are orthogonal facts, both
    // worth showing at once.
    void setRowAccentColour (bool hasAccent, juce::Colour c = juce::Colours::transparentBlack)
    {
        rowHasAccent = hasAccent;
        rowAccentColour = c;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = 8.0f;

        const juce::Colour kCard       (0xff151527);
        const juce::Colour kQueued     (0xffffc933);
        const juce::Colour kPlay       (0xff2ee86a);
        const juce::Colour kTextBright (0xfff2f0ff);
        const juce::Colour kTextDim    (0xffa3a6cc);

        // Visual-polish pass (LoopLab reference): same layered-alpha
        // fake-glow SceneButton/SignatureRailButton already use (JUCE has no
        // cheap real blur) -- a playing row is the single most "this is
        // making sound right now" cue in PERFORM, so it gets the same
        // "lit" treatment those two already established, not a flat wash.
        // Drawn BEHIND the fill/border below.
        if (state == State::playing)
        {
            for (int i = 3; i >= 1; --i)
            {
                const float expand = (float) i * 2.0f;
                g.setColour (kPlay.withAlpha (0.10f / (float) i));
                g.fillRoundedRectangle (bounds.expanded (expand), radius + expand);
            }
        }

        g.setColour (kCard);
        g.fillRoundedRectangle (bounds, radius);

        if (state == State::playing)
        {
            g.setColour (kPlay.withAlpha (0.25f));
            g.fillRoundedRectangle (bounds, radius);
        }

        if (state == State::queued)
        {
            // eases like DeckCard's live pulse, but at 2 Hz -- an
            // attention-grabbing "about to switch" cue, not a beat-locked one
            const float t = (float) pulsePhase;
            const float eased = 1.0f - (t * t);
            const float brightness = juce::jlimit (0.55f, 1.0f, 0.65f + 0.35f * eased);
            g.setColour (kQueued.withBrightness (brightness));
            g.drawRoundedRectangle (bounds, radius, 2.0f);
        }
        else if (state == State::playing)
        {
            g.setColour (kPlay);
            g.drawRoundedRectangle (bounds, radius, 2.0f);
        }
        else if (state == State::armed)
        {
            // Selected + stopped: full colour, outlined -- same border
            // treatment as playing, but no wash fill above and no pulse,
            // so it reads as "armed, not sounding" rather than "live."
            g.setColour (kPlay);
            g.drawRoundedRectangle (bounds, radius, 2.0f);
        }
        else
        {
            g.setColour (juce::Colour (0xff2b2b4d));
            g.drawRoundedRectangle (bounds, radius, 1.0f);
        }

        if (rowHasAccent)
        {
            auto strip = getLocalBounds().toFloat().removeFromLeft (4.0f).reduced (1.0f, 3.0f);
            g.setColour (rowAccentColour);
            g.fillRoundedRectangle (strip, 2.0f);
        }

        // Phase 1.1 P7 "hover": a faint overlay, independent of and on top
        // of whatever idle/queued/playing colouring already applies -- "you
        // are pointing at this," not a second state to track.
        if (hovering)
        {
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.fillRoundedRectangle (bounds, radius);
        }

        // Owner #9 layout: big slot name, signature below, tempo below that,
        // then the stem/loop mode SYMBOL in a tappable pill at the bottom.
        auto area = getLocalBounds().reduced (4, 6);
        const juce::Colour textColour = state == State::queued  ? kQueued
                                       : (state == State::playing || state == State::armed) ? kTextBright
                                                                  : kTextDim;

        // mode symbol pill (bottom) -- reserved first so the text stack
        // above never collides with it. Bounds cached for mouseUp's hit test.
        auto pillArea = area.removeFromBottom (24);
        {
            const float pw = 40.0f, ph = 20.0f;
            modePillBounds = juce::Rectangle<float> (pillArea.toFloat().getCentreX() - pw / 2.0f,
                                                      pillArea.toFloat().getCentreY() - ph / 2.0f, pw, ph);
            g.setColour (juce::Colour (0xff262840).withAlpha (pillHover ? 1.0f : 0.75f));
            g.fillRoundedRectangle (modePillBounds, ph / 2.0f);
            g.setColour (pillHover ? kTextBright.withAlpha (0.6f) : juce::Colour (0xff3a3d5c));
            g.drawRoundedRectangle (modePillBounds, ph / 2.0f, 1.0f);

            const auto c = modePillBounds.getCentre();
            g.setColour (pillHover ? kTextBright : kTextDim);
            if (modeIsStem)
            {
                // one-shot "play to the end bar": right-pointing triangle + bar
                juce::Path p;
                p.addTriangle (c.x - 6.0f, c.y - 4.5f, c.x - 6.0f, c.y + 4.5f, c.x + 2.5f, c.y);
                g.fillPath (p);
                g.fillRoundedRectangle (c.x + 4.5f, c.y - 4.5f, 1.8f, 9.0f, 0.9f);
            }
            else
            {
                // loop: open circle arc with an arrowhead
                juce::Path p;
                p.addCentredArc (c.x, c.y, 4.8f, 4.8f, 0.0f,
                                 0.35f, juce::MathConstants<float>::twoPi * 0.86f, true);
                g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
                const auto tip = c.getPointOnCircumference (4.8f, 0.35f);
                juce::Path arrow;
                arrow.addTriangle (tip.x + 1.4f, tip.y - 3.4f, tip.x + 3.2f, tip.y + 2.2f, tip.x - 2.6f, tip.y + 1.4f);
                g.fillPath (arrow);
            }
        }

        // big slot name -- the owner's "play A1 big (or Name)"
        g.setColour (textColour);
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawText (slotNameText, area.removeFromTop (20), juce::Justification::centred, true);

        g.setColour (juce::Colour (0xffa3a6cc));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (sigText, area.removeFromTop (13), juce::Justification::centred, false);

        g.setColour (juce::Colour (0xff6f7099));
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::plain)));
        g.drawText (tempoText, area.removeFromTop (12), juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onMenu != nullptr) onMenu(); return; }
        touchHold.onLongPress = [this] (juce::Point<int>) { if (onMenu != nullptr) onMenu(); };
        touchHold.begin (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override { touchHold.drag (e); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (touchHold.end()) return;   // the hold already opened the menu
        if (! getLocalBounds().contains (e.getPosition())) return;
        // Owner #9: a tap on the mode pill toggles stem/loop -- it must NOT
        // also trigger the row, so it's checked before onTrigger.
        if (modePillBounds.contains (e.getPosition().toFloat()))
        {
            if (onModeToggle != nullptr) onModeToggle();
            return;
        }
        if (onTrigger != nullptr) onTrigger();
    }

    void mouseEnter (const juce::MouseEvent&) override { hovering = true; repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovering = false; pillHover = false; repaint(); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool over = modePillBounds.contains (e.getPosition().toFloat());
        if (over != pillHover) { pillHover = over; repaint(); }
    }

private:
    State state { State::idle };
    double pulsePhase { 0.0 };
    juce::String slotNameText, sigText, tempoText;
    bool modeIsStem { false };
    juce::Rectangle<float> modePillBounds;
    bool pillHover { false };
    bool rowHasAccent { false };
    juce::Colour rowAccentColour { juce::Colours::transparentBlack };
    bool hovering { false };
    eztouch::LongPress touchHold;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckTriggerCell)
};

//==============================================================================
//  UI_SPEC_PERFORM.md §2.5 (build-order step 5) -- one Pads/FX slot. Replaces
//  DeckTriggerButton inside VoiceBankPanel (DeckTriggerButton itself is now
//  fully unused and removed -- nothing else referenced it).
//
//  KNOWN GAP, not approximated: §2.5's "Playing" state also wants a 3px
//  kPlay progress bar "tracking position" along the bottom edge.
//  OneShotVoice.h (engine, off-limits this pass) exposes isActive() but no
//  playhead-fraction accessor -- there is nothing to read a real position
//  from without adding one. The 2px kPlay BORDER (driven by isActive(),
//  already exposed) is implemented; the moving bar is deliberately left
//  out rather than faked with a fake/static fill, pending a future
//  OneShotVoice::getProgress()-style accessor.
//==============================================================================
class VoiceSlotCell : public juce::Component,
                      public juce::DragAndDropTarget,       // Library card drags ("ezplay-asset:" -- DeckCard's own vocabulary, reused)
                      public juce::FileDragAndDropTarget    // OS file drags (owner: "the pads and FX should also accept drag and drop")
{
public:
    enum class State { empty, loaded, playing };

    // Explicit, not implicit -- JUCE_DECLARE_NON_COPYABLE below deletes the
    // copy constructor, which (per the standard) suppresses the implicit
    // default constructor too. Same fix as DeckTriggerCell's own comment.
    VoiceSlotCell() = default;

    std::function<void()> onTap;
    std::function<void()> onMenu;
    // SPEC_PERFORM_V2 GROUP H2: tapping an empty slot routes here -- opens
    // the Library to load into this exact slot. (The "..." corner glyph
    // that also reached this path is gone -- owner: not touch friendly;
    // a loaded slot's load/replace lives in the slot editor instead.)
    std::function<void()> onLibraryTap;
    // SPEC_PERFORM_V2 GROUP F (103c): opens VoiceEditorContent in the shared
    // bottom dock -- same double-click gesture DeckCard's own onDoubleTap
    // already establishes for decks, applied here for consistency.
    std::function<void()> onDoubleTap;
    // Owner request: pads/FX accept drops -- a Library card (assetId, same
    // "ezplay-asset:" vocabulary DeckCard already recognises) or OS files.
    std::function<void (const juce::String&)>      onAssetDropped;
    std::function<void (const juce::StringArray&)> onFilesDropped;

    void setState (State s)      { if (state != s) { state = s; repaint(); } }
    void setIndex (int i)        { index = i; repaint(); }
    void setBoundKey (const juce::String& k) { boundKey = k; repaint(); }
    // Owner: "show a little light animation for the pad that's currently
    // playing" -- a slow breathing pulse pushed in from the 15Hz UI timer
    // (same push-only idiom as DeckTriggerCell::setPulsePhase). Repaints
    // only while playing, so resting/empty slots cost nothing.
    void setPulsePhase (double phase01)
    {
        pulsePhase = juce::jlimit (0.0, 1.0, phase01);
        if (state == State::playing) repaint();
    }
    void setClipName (const juce::String& n) { clipName = n; repaint(); }
    void setDurationText (const juce::String& d) { durationText = d; repaint(); }
    // SPEC_PERFORM_V2 GROUP F (103c): per-slot colour, PerformLive UI/UX
    // Design Notes' "each pad supports... Color." A thin left-edge accent
    // stripe, same idiom DeckCard's own setAccent()/paint() already use for
    // deck layer identity colour -- "reuse the exact same editor/styling."
    void setAccentColour (bool set, juce::Colour c) { accentSet = set; accentColour = c; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = 8.0f;

        // Hardcoded hex, not performlive:: -- same reason as DeckTriggerCell
        // above (this file's palette namespace is declared after
        // SessionComponent's own forward point).
        const juce::Colour kSlotEmpty  (0xff0f0f1d);
        const juce::Colour kCard       (0xff151527);
        const juce::Colour kBorder     (0xff2b2b4d);
        const juce::Colour kPlay       (0xff2ee86a);
        const juce::Colour kTextFaint  (0xff6f7099);
        const juce::Colour kTextBright (0xfff2f0ff);

        // Owner #6: "colour the whole pad in a liquid glass feel... colours
        // that light up when playing." A coloured slot is filled edge-to-edge
        // with its OWN colour (dimmed at rest, lit + glowing while playing)
        // -- replacing the old 3px left-edge stripe. Uncoloured slots keep
        // the previous card look with the green playing treatment.
        const bool glass = accentSet && state != State::empty;
        const juce::Colour glowColour = glass ? accentColour : kPlay;

        // Owner: "show a little light animation for the pad that's currently
        // playing" -- a slow breathing pulse (0..1, eased sine) over the glow
        // strength and the lit fill, phase pushed in from the 15Hz UI timer.
        const float pulse = state == State::playing
                              ? 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * (float) pulsePhase)
                              : 0.0f;

        // Same fake-glow idiom as DeckTriggerCell/DeckCard's "live" state
        // (JUCE has no cheap real blur) -- but in the slot's own colour,
        // breathing with the pulse.
        if (state == State::playing)
        {
            for (int i = 3; i >= 1; --i)
            {
                const float expand = (float) i * (2.0f + 1.5f * pulse);
                g.setColour (glowColour.withAlpha ((0.11f + 0.07f * pulse) / (float) i));
                g.fillRoundedRectangle (bounds.expanded (expand), radius + expand);
            }
        }

        if (glass)
        {
            const bool live = (state == State::playing);
            // body: vertical gradient of the slot colour -- resting pads sit
            // darker/duller so the lit (playing) state visibly "lights up",
            // and the lit top edge breathes with the pulse
            const juce::Colour top = live ? accentColour.brighter (0.35f + 0.20f * pulse).withMultipliedSaturation (1.10f)
                                          : accentColour.darker (0.22f).withMultipliedSaturation (1.15f);
            const juce::Colour bot = live ? accentColour.darker (0.15f).withMultipliedSaturation (1.10f)
                                          : accentColour.darker (0.74f).withMultipliedSaturation (1.15f);
            g.setGradientFill (juce::ColourGradient (top, bounds.getX(), bounds.getY(),
                                                      bot, bounds.getX(), bounds.getBottom(), false));
            g.fillRoundedRectangle (bounds, radius);

            // the "glass": a white sheen washing down the top half, clipped
            // to the rounded body
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip;
            clip.addRoundedRectangle (bounds, radius);
            g.reduceClipRegion (clip);
            auto sheen = bounds.withHeight (bounds.getHeight() * 0.48f);
            const auto sheenTint = accentColour.brighter (0.95f);
            g.setGradientFill (juce::ColourGradient (sheenTint.withAlpha (live ? 0.30f : 0.12f),
                                                      sheen.getX(), sheen.getY(),
                                                      sheenTint.withAlpha (0.0f),
                                                      sheen.getX(), sheen.getBottom(), false));
            g.fillRect (sheen);
        }
        else
        {
            g.setColour (state == State::empty ? kSlotEmpty : kCard);
            g.fillRoundedRectangle (bounds, radius);
        }

        if (state == State::empty)
        {
            juce::Path outline, dashed;
            outline.addRoundedRectangle (bounds, radius);
            const float dashes[] { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (kBorder);
            g.strokePath (dashed, juce::PathStrokeType (1.0f));
        }
        else if (glass)
        {
            g.setColour (state == State::playing ? accentColour.brighter (0.8f)
                                                  : accentColour.brighter (0.15f).withAlpha (0.9f));
            g.drawRoundedRectangle (bounds, radius, state == State::playing ? 2.0f : 1.0f);
        }
        else
        {
            g.setColour (state == State::playing ? kPlay : kBorder);
            g.drawRoundedRectangle (bounds, radius, state == State::playing ? 2.0f : 1.0f);
        }

        // Text must stay readable on ANY of the eight swatch colours: bright
        // fills (yellow, teal...) get near-black text, dark fills keep white.
        const bool darkText = glass && accentColour.getPerceivedBrightness() > 0.62f;
        const juce::Colour primaryText   = glass ? (darkText ? juce::Colour (0xff10121c) : juce::Colours::white)
                                                  : kTextBright;
        const juce::Colour secondaryText = glass ? (darkText ? juce::Colour (0xff10121c).withAlpha (0.72f)
                                                              : juce::Colours::white.withAlpha (0.75f))
                                                  : kTextFaint;

        auto area = getLocalBounds().reduced (5, 3);

        // Owner: "I don't like the 3 dots by the pads and FX, take it off"
        // -- the "..." glyph is gone (its functions live in the slot editor,
        // reached by right-click/double-click); boundKey has the full
        // top-right again.
        g.setColour (secondaryText);
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::plain)));
        auto topRow = area.removeFromTop (12);
        g.drawText (juce::String (index), topRow, juce::Justification::topLeft, false);
        if (state != State::empty)
            g.drawText (boundKey, topRow, juce::Justification::topRight, false);

        if (state == State::empty)
        {
            g.setColour (kTextFaint);
            g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::plain)));
            g.drawText ("+", getLocalBounds(), juce::Justification::centred, false);
            if (dragHover)   // empty slots are the primary drop targets -- same cue as below
            {
                g.setColour (juce::Colour (0xff7c5cff).withAlpha (0.18f));
                g.fillRoundedRectangle (bounds, radius);
                g.setColour (juce::Colour (0xff7c5cff));
                g.drawRoundedRectangle (bounds, radius, 2.0f);
            }
            return;
        }

        auto bottomRow = area.removeFromBottom (11);
        g.setColour (secondaryText);
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::plain)));
        g.drawText (durationText, bottomRow, juce::Justification::bottomRight, false);

        g.setColour (primaryText);
        g.setFont (juce::Font (juce::FontOptions (11.0f, glass ? juce::Font::bold : juce::Font::plain)));
        g.drawText (clipName, area, juce::Justification::centred, true);

        // Phase 1.1 P7 "hover" -- same faint overlay idiom as
        // DeckTriggerCell's own (matching hover treatment across every
        // trigger-style cell in PERFORM, not a one-off).
        if (hovering)
        {
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.fillRoundedRectangle (bounds, radius);
        }

        // drop-target highlight -- same indigo "drop here" cue DeckCard
        // shows during a drag, so pads/FX read as droppable too.
        if (dragHover)
        {
            g.setColour (juce::Colour (0xff7c5cff).withAlpha (0.18f));
            g.fillRoundedRectangle (bounds, radius);
            g.setColour (juce::Colour (0xff7c5cff));
            g.drawRoundedRectangle (bounds, radius, 2.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onMenu != nullptr) onMenu(); return; }
        touchHold.onLongPress = [this] (juce::Point<int>) { if (onMenu != nullptr) onMenu(); };
        touchHold.begin (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override { touchHold.drag (e); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (touchHold.end()) return;
        if (! getLocalBounds().contains (e.getPosition())) return;

        // An empty slot's whole area IS its "+" (mirrors DeckCard's own
        // empty-state tap) -- opens the Library to load into this slot. A
        // loaded slot's tap plays it; replacing its audio lives in the slot
        // editor (right-click / double-click). The old "..." corner glyph
        // is gone (owner: not touch friendly).
        if (state == State::empty)
        {
            if (onLibraryTap != nullptr) onLibraryTap();
            return;
        }

        // Owner #10: deferred single-tap so a double-click (open editor)
        // doesn't ALSO trigger the pad -- same serial-cancel idiom as
        // DeckCard::mouseUp (see its comment).
        ++pendingTapSerial;
        const int serial = pendingTapSerial;
        juce::Component::SafePointer<VoiceSlotCell> safe (this);
        juce::Timer::callAfterDelay (220, [safe, serial]
        {
            if (safe != nullptr && safe->pendingTapSerial == serial && safe->onTap != nullptr)
                safe->onTap();
        });
    }

    void mouseEnter (const juce::MouseEvent&) override { hovering = true; repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovering = false; repaint(); }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        ++pendingTapSerial;   // cancel the deferred single-tap (owner #10)
        if (state != State::empty && onDoubleTap != nullptr) onDoubleTap();
    }

    // Owner #10 -- public for the deferred-tap lambda, same as DeckCard's.
    int pendingTapSerial { 0 };

    //==========================================================================
    //  juce::DragAndDropTarget -- Library cards (DeckCard's own vocabulary)
    //==========================================================================
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return details.description.toString().startsWith ("ezplay-asset:");
    }
    void itemDragEnter (const SourceDetails&) override { dragHover = true; repaint(); }
    void itemDragExit  (const SourceDetails&) override { dragHover = false; repaint(); }
    void itemDropped   (const SourceDetails& details) override
    {
        dragHover = false; repaint();
        if (onAssetDropped != nullptr)
            onAssetDropped (details.description.toString().fromFirstOccurrenceOf ("ezplay-asset:", false, false));
    }

    //==========================================================================
    //  juce::FileDragAndDropTarget -- OS files (Explorer/desktop)
    //==========================================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return anySupportedAudioPath (files); }
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit  (const juce::StringArray&) override           { dragHover = false; repaint(); }
    void filesDropped  (const juce::StringArray& files, int, int) override
    {
        dragHover = false; repaint();
        if (onFilesDropped != nullptr) onFilesDropped (files);
    }

private:
    State state { State::empty };
    int index { 1 };
    juce::String boundKey, clipName, durationText;
    bool hovering { false };
    bool dragHover { false };
    bool accentSet { false };
    juce::Colour accentColour { juce::Colours::transparentBlack };
    double pulsePhase { 0.0 };   // owner: playing-pad light animation
    eztouch::LongPress touchHold;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceSlotCell)
};

//==============================================================================
//  UI_SPEC_MIXER.md §3.3: a fader-only LookAndFeel -- LookAndFeel_V4's own
//  drawLinearSlider() renders a circular thumb with no per-slider control
//  over track width, which can't reach the spec's literal 4px track / 20px
//  rounded-rect thumb. One instance is shared by every strip's slider
//  (LookAndFeel objects are meant to be shared, not one per Component) via
//  the function-local static below, avoiding any global-init-order risk.
//==============================================================================
//==============================================================================
//  Bug report: "something still not right with the ui" -- comparing against
//  the original LoopLab reference mockup, the gap wasn't missing features
//  (this app already matches or exceeds it feature-for-feature) but visual
//  language: every plain TextButton here was a flat single-colour rectangle
//  with no depth and no press feedback, where the reference gives every
//  control a consistent "raised glass key" treatment -- a top-lit gradient,
//  an inset highlight along the top edge and an inset shadow along the
//  bottom (a bevel), a soft ambient drop shadow, and a real sunken look when
//  held. This LookAndFeel reproduces that for ordinary TextButtons, reading
//  its base colour from whatever buttonColourId/buttonOnColourId the caller
//  already set (backgroundColour is JUCE's own already-resolved on/off/
//  highlighted colour) -- no call site needs its own colour setup touched.
//  One shared instance via a function-local static, same reason
//  MixerFaderLookAndFeel below does the same (LookAndFeel objects are meant
//  to be shared, not one per Component).
//==============================================================================
class TactileButtonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const float radius = juce::jmin (8.0f, bounds.getHeight() * 0.35f);

        // A fully transparent base (e.g. addSignatureButton/addSceneButton's
        // deliberately chromeless "+") stays chromeless -- no gradient, no
        // bevel, no shadow to paint over nothing.
        if (backgroundColour.getAlpha() == 0)
            return;

        // Ambient drop shadow: a real (not faked) blur via juce::DropShadow,
        // omitted while held -- a sunk key casts no shadow of its own.
        if (! shouldDrawButtonAsDown)
        {
            juce::Path shadowPath;
            shadowPath.addRoundedRectangle (bounds, radius);
            juce::DropShadow (juce::Colours::black.withAlpha (0.4f), 5, { 0, 2 }).drawForPath (g, shadowPath);
        }

        auto base = shouldDrawButtonAsHighlighted ? backgroundColour.brighter (0.12f) : backgroundColour;
        if (shouldDrawButtonAsDown) base = base.darker (0.18f);

        juce::ColourGradient grad (base.brighter (0.16f), bounds.getX(), bounds.getY(),
                                    base.darker (0.10f), bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (bounds, radius);

        // Bevel: inset highlight along the top edge, inset shadow along the
        // bottom -- both dimmed and the highlight nearly gone when held, so
        // a pressed button reads as recessed rather than raised.
        const float inset = radius * 0.4f;
        g.setColour (juce::Colours::white.withAlpha (shouldDrawButtonAsDown ? 0.03f : 0.16f));
        g.drawLine (bounds.getX() + inset, bounds.getY() + 1.0f, bounds.getRight() - inset, bounds.getY() + 1.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (shouldDrawButtonAsDown ? 0.4f : 0.22f));
        g.drawLine (bounds.getX() + inset, bounds.getBottom() - 1.0f, bounds.getRight() - inset, bounds.getBottom() - 1.0f, 1.0f);

        g.setColour (juce::Colour (0xff2b2b4d).withAlpha (0.85f));   // kBorder
        g.drawRoundedRectangle (bounds, radius, 1.0f);
    }

    // Same bevel/gradient language as drawButtonBackground above, applied to
    // ComboBox (the mixer's own output-route selector) so a dropdown reads
    // as the same kind of raised control as everything around it, not a
    // visually distinct flat box.
    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                        int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox& box) override
    {
        juce::ignoreUnused (buttonX, buttonY, buttonW, buttonH);
        auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        const float radius = juce::jmin (6.0f, bounds.getHeight() * 0.3f);

        auto base = box.findColour (juce::ComboBox::backgroundColourId);
        if (isButtonDown) base = base.darker (0.18f);

        juce::ColourGradient grad (base.brighter (0.14f), bounds.getX(), bounds.getY(),
                                    base.darker (0.08f), bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (juce::Colour (0xff2b2b4d).withAlpha (0.85f));   // kBorder
        g.drawRoundedRectangle (bounds, radius, 1.0f);

        auto arrowZone = bounds.removeFromRight (bounds.getHeight()).reduced (bounds.getHeight() * 0.28f);
        juce::Path arrow;
        arrow.startNewSubPath (arrowZone.getX(), arrowZone.getY());
        arrow.lineTo (arrowZone.getCentreX(), arrowZone.getBottom());
        arrow.lineTo (arrowZone.getRight(), arrowZone.getY());
        g.setColour (box.findColour (juce::ComboBox::textColourId).withAlpha (0.8f));
        g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};

inline juce::LookAndFeel& tactileButtonLookAndFeel()
{
    static TactileButtonLookAndFeel instance;
    return instance;
}

//==============================================================================
//  PerformLive UI/UX Design Notes (Studio One reference), SPEC_PERFORM_V2
//  GROUP F (104): "Toolbar icons (especially metronome, volume, 'more
//  options'/gear) should be redesigned to a modern, consistent icon family."
//  The volume/speaker icon (SessionComponent::paint()) is already a real
//  hand-drawn juce::Path, not a text glyph -- consistent with that, this is
//  a plain juce::TextButton subclass that keeps every existing click/hover/
//  toggle/tooltip/LookAndFeel behaviour (paintButton() calls straight
//  through to the current LookAndFeel's drawButtonBackground(), same as
//  TextButton's own default paintButton()) but draws a small vector icon
//  instead of a text glyph -- replacing metronomeButton's plain "M" and
//  settingsButton's U+2699 GEAR character (a font glyph this app's chosen
//  font doesn't actually contain, falling back to a dot-cluster glyph --
//  the exact "more options"/gear this task exists to fix).
//==============================================================================
class IconGlyphButton : public juce::TextButton
{
public:
    enum class Glyph { metronome, gear };

    explicit IconGlyphButton (Glyph g) : glyph (g) {}

    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        getLookAndFeel().drawButtonBackground (g, *this, findColour (juce::TextButton::buttonColourId),
                                                shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

        auto bounds = getLocalBounds().toFloat().reduced (getWidth() * 0.26f, getHeight() * 0.26f);
        g.setColour (findColour (juce::TextButton::textColourOffId));
        if (glyph == Glyph::metronome) drawMetronome (g, bounds);
        else                            drawGear (g, bounds);
    }

private:
    // A simple triangular metronome body (wide base, narrow top) with a
    // pendulum arm swinging slightly off-centre and a small weight -- the
    // same silhouette Studio One's own metronome icon uses, drawn as plain
    // strokes/fills rather than an embedded image asset.
    static void drawMetronome (juce::Graphics& g, juce::Rectangle<float> b)
    {
        juce::Path body;
        body.startNewSubPath (b.getCentreX(), b.getY());
        body.lineTo (b.getRight(), b.getBottom());
        body.lineTo (b.getX(), b.getBottom());
        body.closeSubPath();
        g.strokePath (body, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // base plinth
        g.fillRect (b.getX(), b.getBottom() - 1.5f, b.getWidth(), 1.5f);

        // pendulum arm, tilted right of centre, plus a small round weight --
        // a static tilt (no animation) reads clearly as "metronome" at
        // toolbar size without competing with the RGB ring/beat-pulse
        // animations elsewhere in the app.
        const auto pivot  = juce::Point<float> (b.getCentreX(), b.getBottom() - 2.0f);
        const auto tip    = juce::Point<float> (b.getCentreX() + b.getWidth() * 0.20f, b.getY() + b.getHeight() * 0.12f);
        const auto weight = pivot + (tip - pivot) * 0.68f;   // 68% of the way up the arm, precisely on the line
        g.drawLine ({ pivot, tip }, 1.4f);
        g.fillEllipse (juce::Rectangle<float> (3.4f, 3.4f).withCentre (weight));
    }

    // A jagged 8-tooth gear silhouette with a punched-out centre hole
    // (even-odd fill rule: the outer star path and the inner circle
    // together leave only the ring/teeth filled) -- JUCE has no built-in
    // gear path, so this is the standard "star polygon + hole" technique.
    static void drawGear (juce::Graphics& g, juce::Rectangle<float> b)
    {
        constexpr int numTeeth = 8;
        const auto centre = b.getCentre();
        const float outerR = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        const float innerR = outerR * 0.62f;
        const float holeR  = outerR * 0.34f;

        juce::Path gear;
        for (int i = 0; i < numTeeth * 2; ++i)
        {
            const float angle = (float) i / (float) (numTeeth * 2) * juce::MathConstants<float>::twoPi;
            const float r = (i % 2 == 0) ? outerR : innerR;
            const auto p = centre.getPointOnCircumference (r, angle);
            if (i == 0) gear.startNewSubPath (p); else gear.lineTo (p);
        }
        gear.closeSubPath();
        gear.addEllipse (juce::Rectangle<float> (holeR * 2.0f, holeR * 2.0f).withCentre (centre));
        gear.setUsingNonZeroWinding (false);   // even-odd -- the ellipse subtracts, rather than adds to, the star
        g.fillPath (gear);
    }

    Glyph glyph;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconGlyphButton)
};

class MixerFaderLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider&) override
    {
        juce::ignoreUnused (minSliderPos, maxSliderPos, style);

        const juce::Colour kIndigoDim  (0xff3a2f6b);
        const juce::Colour kIndigo     (0xff7c5cff);
        const juce::Colour kTextBright (0xfff2f0ff);
        const juce::Colour kBorder     (0xff2b2b4d);

        const float trackW = 4.0f;
        const float cx = (float) x + (float) width * 0.5f;
        auto track = juce::Rectangle<float> (cx - trackW * 0.5f, (float) y, trackW, (float) height);

        g.setColour (kIndigoDim);
        g.fillRoundedRectangle (track, trackW * 0.5f);

        // filled portion -- from the current value down to the track's own
        // bottom, since sliderPos is already the y of the thumb centre for a
        // LinearVertical slider (JUCE convention: smaller y = larger value).
        g.setColour (kIndigo);
        g.fillRoundedRectangle (track.withTop (sliderPos), trackW * 0.5f);

        // Visual-polish pass (LoopLab reference): a round ball thumb with a
        // top-lit indigo gradient, matching the reference's own fader thumb
        // exactly -- was a flat rounded-rect square.
        const float thumbSize = 18.0f;
        auto thumb = juce::Rectangle<float> (cx - thumbSize * 0.5f, sliderPos - thumbSize * 0.5f, thumbSize, thumbSize);
        juce::ColourGradient thumbGrad (juce::Colour (0xffa5b4fc), thumb.getX(), thumb.getY(),
                                         kIndigo, thumb.getX(), thumb.getBottom(), false);
        g.setGradientFill (thumbGrad);
        g.fillEllipse (thumb);
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.drawEllipse (thumb.reduced (1.0f), 1.0f);
    }
};

//==============================================================================
//  Milestone 7: one mixer channel strip -- a vertical gain slider plus
//  mute/solo toggle buttons, wired directly to a specific ezdeck::Mixer
//  channel via simple onValueChange/onClick lambdas (no Button::Listener
//  indirection needed for this self-contained new component). Master uses
//  just the slider (no mute/solo -- it's the post-sum stage, not one of the
//  7 channels, per PRODUCT_REQUIREMENTS.md §10 and ARCHITECTURE.md's
//  resolved Architecture Decision Pending #4).
//
//  UI_SPEC_MIXER.md §7 (build-order step 1): restyled per §3 -- frame, name,
//  fader, meter, value readout, M/S. Every existing callback signature
//  (onGainChanged/onMuteChanged/onSoloChanged/setMeterLevel/setMeterVisible)
//  is untouched; this is a paint/layout restyle, not a rewire. The
//  constructor gained an accent colour + isTabChannel (both plumbing the
//  SessionComponent already computes from performlive::columnAccent(), not
//  a new concept) and setSilencedByOtherSolo()/setArmedIndicator() are new,
//  additive methods -- see their own comments for why each is needed.
//==============================================================================
class MixerChannelStrip : public juce::Component
{
public:
    // Visual-polish pass (LoopLab reference): Metro/Master get their own
    // tinted strip background in the reference (amber/indigo washes) instead
    // of every strip reading identically -- tintColour is that wash,
    // transparent (the default) for every ordinary channel strip.
    MixerChannelStrip (const juce::String& label, juce::Colour accentIn, bool isTabChannelIn, bool showMuteSoloIn,
                        juce::Colour tintColourIn = juce::Colours::transparentBlack)
        : channelName (label), accent (accentIn), isTabChannel (isTabChannelIn), showMuteSolo (showMuteSoloIn),
          tintColour (tintColourIn)
    {
        touchHold.onLongPress = [this] (juce::Point<int>) { if (onStripMenu) onStripMenu(); };
        gainSlider.setLookAndFeel (&faderLookAndFeel());
        gainSlider.setSliderStyle (juce::Slider::LinearVertical);
        gainSlider.setRange (0.0, 1.5, 0.01);
        gainSlider.setValue (1.0, juce::dontSendNotification);
        gainSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);   // §3.3: the value readout below replaces it
        gainSlider.setDoubleClickReturnValue (true, 1.0);                   // §3.3: double-click resets to 1.0 (matches v20)
        gainSlider.onValueChange = [this]
        {
            if (onGainChanged != nullptr) onGainChanged ((float) gainSlider.getValue());
            repaint();   // §3.4: value readout updates live with the fader
        };
        addAndMakeVisible (gainSlider);

        if (showMuteSolo)
        {
            muteButton.setButtonText ("M");
            muteButton.setClickingTogglesState (true);
            muteButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527));   // kCard
            muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffff3b5c));   // kDanger
            muteButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffa3a6cc));   // kTextDim
            muteButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xfff2f0ff));   // kTextBright
            muteButton.setLookAndFeel (&tactileButtonLookAndFeel());
            muteButton.onClick = [this]
            {
                if (onMuteChanged != nullptr) onMuteChanged (muteButton.getToggleState());
                refreshDim();
            };
            addAndMakeVisible (muteButton);

            soloButton.setButtonText ("S");
            soloButton.setClickingTogglesState (true);
            soloButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527));   // kCard
            soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffc933));   // kMeterMid/amber
            soloButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffa3a6cc));   // kTextDim
            soloButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff0c0c17));
            soloButton.setLookAndFeel (&tactileButtonLookAndFeel());
            soloButton.onClick = [this] { if (onSoloChanged != nullptr) onSoloChanged (soloButton.getToggleState()); repaint(); };
            addAndMakeVisible (soloButton);

            // Phase 1.1 P3 "Add Output routing: every channel selects Main /
            // Output 1-4 / custom buses; future-proof for multiple
            // interfaces." Gated on showMuteSolo (same "a real routable
            // channel, not Master" condition M/S already use) -- Master IS
            // the final output stage by definition, not itself re-routable.
            // SPEC_OUTPUT_ROUTING.md: this selector and its persistence were
            // already fully built by Phase 1.1 P3, with exactly one
            // documented gap ("selecting anything other than Main is
            // stored but doesn't actually re-route audio yet since the
            // engine has one output bus") -- that gap is what this task
            // closes (SessionComponent's own routeIndexToOutputPair() +
            // Mixer::setChannelOutputPair()). "Custom Bus" still has no
            // real engine-level bus concept (falls back to Main, honestly
            // toasted by the caller) -- see routeIndexToOutputPair()'s own
            // comment. Always visible on channel strips (the UI timer's
            // routing block explains why it no longer hides on stereo).
            // Labelled the way the back of the interface is labelled. Index 0
            // is hardware Out 1/2 -- also the Master bus, hence "Main"; index
            // N is hardware pair N+1, and goes straight to those jacks
            // WITHOUT passing through Master. The old labels ("Output 1..4")
            // were off by one from the box: "Output 1" was actually Out 3/4,
            // which is exactly the kind of thing that gets discovered on
            // stage. "Custom Bus" was a placeholder with no engine behind it
            // and is gone.
            // Main + 11 direct pairs (up to Out 23/24): enough for every one
            // of the 11 channels to have its own stereo out on a 24-output
            // interface. Same count as ProjectFile's kMaxOutputRoutes.
            outputRouteBox.addItem ("Main", 1);   // "Main (Out 1/2)" truncated to "Main ..." at strip width
            for (int i = 1; i < ezproject::MixerChannelSnapshot::kMaxOutputRoutes; ++i)
                outputRouteBox.addItem ("Out " + juce::String (2 * i + 1) + "/" + juce::String (2 * i + 2), i + 1);
            outputRouteBox.setSelectedItemIndex (0, juce::dontSendNotification);
            outputRouteBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff151527));   // kCard
            outputRouteBox.setColour (juce::ComboBox::textColourId, juce::Colour (0xffa3a6cc));          // kTextDim
            outputRouteBox.setLookAndFeel (&tactileButtonLookAndFeel());
            outputRouteBox.onChange = [this] { if (onOutputRouteChanged != nullptr) onOutputRouteChanged (outputRouteBox.getSelectedItemIndex()); };
            addAndMakeVisible (outputRouteBox);
        }
    }

    ~MixerChannelStrip() override
    {
        sourceButton.setLookAndFeel (nullptr);
        gainSlider.setLookAndFeel (nullptr);
        muteButton.setLookAndFeel (nullptr);
        soloButton.setLookAndFeel (nullptr);
        outputRouteBox.setLookAndFeel (nullptr);
    }

    std::function<void (float)> onGainChanged;
    std::function<void (bool)>  onMuteChanged;
    std::function<void (bool)>  onSoloChanged;
    std::function<void (int)>   onOutputRouteChanged;   // index into this strip's own kOutputRoutes
    std::function<void()>       onFxToggle;             // the FX pill (DECK 5-8's PERFORM LIVE strip) was tapped

    // Owner: "on the mixer I want to see the channel strip, a sign showing FX
    // that I can click to turn it on or off." Shown only on the columns that
    // have a strip; lit when it's on.
    void setFxState (bool available, bool on)
    {
        if (fxAvailable == available && fxOn == on) return;
        fxAvailable = available; fxOn = on;
        repaint();
    }

    // The WEB strip has no solo and no output routing: it sets a web page's
    // level, it doesn't carry audio through the mixer.
    void setSoloAvailable (bool s) { soloButton.setVisible (s); }
    void setRoutable (bool r)      { routable = r; if (! r) outputRouteBox.setVisible (false); }

    std::function<void()> onStripMenu;       // right-click / hold: the channel strip's menu
    std::function<void()> onSourceClicked;   // live tracks: the source button

    /** Live tracks only: shows the source button ("Mic  In 3", "EZkeys 2", "Choose source"). */
    void setSourceText (const juce::String& text)
    {
        if (! hasSource)
        {
            hasSource = true;
            sourceButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff0c0c17));
            sourceButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f0ff));
            sourceButton.setLookAndFeel (&tactileButtonLookAndFeel());
            sourceButton.onClick = [this] { if (onSourceClicked) onSourceClicked(); };
            addAndMakeVisible (sourceButton);
            resized();
        }
        if (sourceButton.getButtonText() != text) sourceButton.setButtonText (text);
        sourceButton.setTooltip (text);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onStripMenu) onStripMenu(); return; }
        if (fxAvailable && fxPillArea.contains (e.getPosition()) && onFxToggle) { onFxToggle(); return; }
        touchHold.begin (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { touchHold.drag (e); }
    void mouseUp (const juce::MouseEvent&) override    { touchHold.end(); }

    void setOutputRouteIndex (int index) { outputRouteBox.setSelectedItemIndex (juce::jlimit (0, ezproject::MixerChannelSnapshot::kMaxOutputRoutes - 1, index), juce::dontSendNotification); }
    int  getOutputRouteIndex() const     { return outputRouteBox.getSelectedItemIndex(); }
    juce::String outputRouteLabel (int index) const { return outputRouteBox.getItemText (index); }

    // Harmless no-op on the Master strip (showMuteSolo == false there, so
    // outputRouteBox was never added as a visible child at all -- setVisible
    // on a component with no parent just does nothing observable).
    void setOutputRouteControlVisible (bool shown) { outputRouteBox.setVisible (shown && showMuteSolo && routable); }

    // Milestone 10: a lightweight post-fader level meter -- level is
    // expected in [0,1] (PRODUCT_REQUIREMENTS.md §10's own perceptual
    // formula already clamps it there). Set from the message-thread-side UI
    // timer, never from the audio thread.
    //
    // Phase 1.1 P7 "meter easing": this used to assign meterLevel directly,
    // so it visually snapped to a new value every 15Hz tick -- distracting,
    // and unlike every real meter ball/console, which rises instantly on a
    // transient but decays gently rather than dropping in one step. Instant
    // attack (never masks a peak), eased release only -- standard
    // peak-meter ballistics, not a novel animation.
    void setMeterLevel (float level)
    {
        const float target = juce::jlimit (0.0f, 1.0f, level);
        meterLevel = target > meterLevel ? target : meterLevel + (target - meterLevel) * 0.35f;
        repaint();
    }

    // Milestone 13: PRD §14's "Mixer level meters" General-tab toggle.
    void setMeterVisible (bool visible) { meterShown = visible; repaint(); }
    void setMuteState (bool muted)      { muteButton.setToggleState (muted, juce::dontSendNotification); refreshDim(); }
    void setGainValue (float g)         { gainSlider.setValue (g, juce::dontSendNotification); repaint(); }

    // UI_SPEC_MIXER.md §3.1/§3.3: pushed by SessionComponent whenever ANY
    // channel's solo changes -- a single strip has no way to know another
    // channel just soloed. Mirrors Mixer::renderBlock()'s own
    // "soloActive && !solo && !exemptFromSolo" silencing law, computed in
    // SessionComponent via Mixer's existing public getChannelSolo()/
    // isExemptFromSolo() getters -- no engine file touched.
    void setSilencedByOtherSolo (bool s) { silencedByOtherSolo = s; refreshDim(); }

    // UI_SPEC_MIXER.md §3.2: Metro strip's small "armed" dot, pushed from the
    // 15Hz UI timer alongside the existing metronomeButton colour refresh --
    // reusing that timer, not a new one.
    void setArmedIndicator (bool armed) { if (armedIndicator != armed) { armedIndicator = armed; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const bool soloed = showMuteSolo && soloButton.getToggleState();

        // ---- §3.1 frame ----
        // Visual-polish pass (LoopLab reference): a real ambient drop shadow
        // plus a top-lit gradient fill instead of a flat kCard rectangle --
        // same "raised glass" language TactileButtonLookAndFeel established
        // for buttons, inlined here since a Component's own paint() can't
        // borrow a LookAndFeel's drawButtonBackground().
        {
            juce::Path shadowPath;
            shadowPath.addRoundedRectangle (bounds, 10.0f);
            juce::DropShadow (juce::Colours::black.withAlpha (0.35f), 6, { 0, 2 }).drawForPath (g, shadowPath);
        }
        const juce::Colour base (0xff151527);   // kCard
        juce::ColourGradient frameGrad (base.brighter (0.10f), bounds.getX(), bounds.getY(),
                                         base.darker (0.08f), bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill (frameGrad);
        g.fillRoundedRectangle (bounds, 10.0f);

        // Metro/Master's own tinted wash (amber/indigo respectively, see
        // constructor's own comment) -- every ordinary channel strip passes
        // a transparent tintColour, so this is a no-op there.
        if (! tintColour.isTransparent())
        {
            g.setColour (tintColour.withAlpha (0.12f));
            g.fillRoundedRectangle (bounds, 10.0f);
        }

        if (soloed)
        {
            g.setColour (accent.withAlpha (0.08f));
            g.fillRoundedRectangle (bounds, 10.0f);
        }
        g.setColour (soloed ? accent : (! tintColour.isTransparent() ? tintColour.withAlpha (0.4f) : juce::Colour (0xff2b2b4d)));   // accent : tint : kBorder
        g.drawRoundedRectangle (bounds, 10.0f, soloed ? 2.0f : 1.0f);

        // ---- §3.2 name ----
        g.setColour (isTabChannel ? accent : juce::Colour (0xfff2f0ff));   // accent : kTextBright
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (channelName, nameArea, juce::Justification::centred, false);

        // the FX pill: the column's PERFORM LIVE strip, tap to switch
        if (fxAvailable)
        {
            const juce::Colour fxColour (0xffff7a45);
            const auto pill = fxPillArea.toFloat();
            if (fxOn)
            {
                g.setColour (fxColour.withAlpha (0.12f));
                g.fillRoundedRectangle (pill.expanded (2.0f), 6.0f);
                g.setColour (fxColour);
                g.fillRoundedRectangle (pill, 4.0f);
                g.setColour (juce::Colours::black);
            }
            else
            {
                g.setColour (juce::Colour (0xff2b2b4d));
                g.drawRoundedRectangle (pill.reduced (0.5f), 4.0f, 1.0f);
                g.setColour (juce::Colour (0xff6f7099));
            }
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)).withExtraKerningFactor (0.08f));
            g.drawText ("FX", fxPillArea, juce::Justification::centred, false);
        }
        if (armedIndicator)
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)), channelName, 0.0f, 0.0f);
            const float textHalfW = ga.getBoundingBox (0, -1, true).getWidth() * 0.5f;
            auto dot = juce::Rectangle<float> (0, 0, 5.0f, 5.0f)
                           .withCentre ({ (float) nameArea.getCentreX() + textHalfW + 8.0f, (float) nameArea.getCentreY() });
            g.setColour (juce::Colour (0xff2ee86a));   // kPlay
            g.fillEllipse (dot);
        }

        // ---- §3.4 value readout ----
        // Phase 1.1 P3 "'100%' label poorly aligned -- improve spacing/
        // typography, reduce visual weight": centred both ways now (valueArea
        // is a thin 16px strip -- Justification::centred already centres
        // vertically within it, but the OLD 11px plain-weight text read as
        // slightly adrift against the bold fader/name above it); a touch
        // smaller and dimmer so it reads as a secondary readout, not
        // competing with the channel name for attention.
        g.setColour (juce::Colour (0xff6f7099).withAlpha (0.85f));   // kTextFaint
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::plain)));
        g.drawText (juce::String (juce::roundToInt (gainSlider.getValue() * 100.0)) + "%",
                    valueArea, juce::Justification::centred, false);

        // ---- §3.3 meter ----
        if (meterShown)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (meterBounds);

            auto filled = meterBounds;
            filled = filled.removeFromBottom ((int) ((float) filled.getHeight() * meterLevel));

            // Gradient fixed to the meter's own full span (bottom=0%,
            // top=100%) so the visible colour always matches the ABSOLUTE
            // level, not the filled fraction -- green up to 72%, blending to
            // amber, hot red above 92% (§3.3's own literal thresholds).
            juce::ColourGradient grad (juce::Colour (0xff2ee86a), (float) meterBounds.getCentreX(), (float) meterBounds.getBottom(),
                                       juce::Colour (0xffff3b5c), (float) meterBounds.getCentreX(), (float) meterBounds.getY(), false);
            grad.addColour (0.72, juce::Colour (0xff2ee86a));   // kMeterLow, held flat to 72%
            grad.addColour (0.92, juce::Colour (0xffffc933));   // kMeterMid at 92%
            g.setGradientFill (grad);
            g.fillRect (filled);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);   // §3.1: 10px inner padding

        nameArea = area.removeFromTop (18);
        fxPillArea = nameArea.removeFromRight (28).withSizeKeepingCentre (26, 16);   // the FX pill, top-right
        if (hasSource)
        {
            area.removeFromTop (4);
            sourceButton.setBounds (area.removeFromTop (26));
            area.removeFromTop (2);
        }

        // Phase 1.1 P3 "M button oversized, S button oversized... reduce
        // visual weight": was a full 44px-tall row (this app's usual
        // touch-target floor elsewhere, e.g. DeckCard/SceneButton/
        // SignatureRailButton's own 44px controls) split into two halves
        // with no gap -- two big, edge-to-edge squares read as heavier than
        // a simple M/S toggle needs to. Shrunk to 30px (JUCE's own
        // LookAndFeel scales TextButton's font to its height, so this
        // shrinks the glyph too, not just the box) with a real gap between
        // them, matching a mute/solo pair's usual proportions on real
        // hardware mixers -- small, tight, secondary controls under the
        // fader, not equal in visual weight to it.
        juce::Rectangle<int> btnRow;
        if (showMuteSolo)
        {
            area.removeFromBottom (4);
            btnRow = area.removeFromBottom (30);
        }

        // Phase 1.1 P3 "Output routing": a compact row just above M/S --
        // Master (showMuteSolo == false) never shows this, matching that
        // same gate on the ComboBox's own construction.
        if (showMuteSolo)
        {
            area.removeFromBottom (4);
            outputRouteBox.setBounds (area.removeFromBottom (20));
        }

        valueArea = area.removeFromBottom (16);

        area.removeFromTop (4);
        area.removeFromBottom (4);

        // §3.3: fader (left) + meter (right), 2px gap, meter 8px wide.
        meterBounds = area.removeFromRight (8);
        area.removeFromRight (2);
        gainSlider.setBounds (area);

        if (showMuteSolo)
        {
            const int half = btnRow.getWidth() / 2;
            muteButton.setBounds (btnRow.removeFromLeft (half).reduced (4, 0));
            btnRow.removeFromLeft (2);
            soloButton.setBounds (btnRow.reduced (2, 0));
        }
    }

private:
    static juce::LookAndFeel& faderLookAndFeel()
    {
        static MixerFaderLookAndFeel instance;
        return instance;
    }

    void refreshDim()
    {
        const bool dimmed = (showMuteSolo && muteButton.getToggleState()) || silencedByOtherSolo;
        gainSlider.setAlpha (dimmed ? 0.45f : 1.0f);   // §3.1: meter (drawn separately in paint()) is unaffected
    }

    juce::String channelName;
    juce::Colour accent;
    bool isTabChannel;
    bool showMuteSolo;
    juce::Colour tintColour;   // Visual-polish pass -- Metro/Master's own strip wash, see constructor's own comment

    juce::Rectangle<int> nameArea, valueArea, meterBounds, fxPillArea;
    float meterLevel { 0.0f };
    bool  meterShown { true };
    bool  silencedByOtherSolo { false };
    bool  armedIndicator { false };
    bool  fxAvailable { false }, fxOn { false };
    bool  routable { true };
    bool  hasSource { false };
    juce::TextButton sourceButton;
    eztouch::LongPress touchHold { };

    juce::Slider       gainSlider;
    juce::TextButton   muteButton, soloButton;
    juce::ComboBox     outputRouteBox;   // Phase 1.1 P3 "Output routing"
};

//==============================================================================
//  Owner: "when the window is smaller the mixer shouldn't rearrange with some
//  coming down -- it should stay straight, with a scroll bar." The strips sit
//  on this, in one row inside a horizontal Viewport, under a label per group
//  (DECKS / LIVE / RETURNS). Master stays outside it, pinned on the right.
//==============================================================================
class MixerStripHolder : public juce::Component
{
public:
    struct Group { juce::String name; juce::Rectangle<int> area; juce::Colour colour; };
    std::vector<Group> groups;

    void paint (juce::Graphics& g) override
    {
        for (const auto& gr : groups)
        {
            auto label = gr.area.withHeight (16);
            g.setColour (gr.colour);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)).withExtraKerningFactor (0.1f));
            g.drawText (gr.name, label.withTrimmedLeft (2), juce::Justification::centredLeft, false);
            const int textW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), gr.name) + 10;
            g.setColour (gr.colour.withAlpha (0.25f));
            g.fillRect (label.getX() + textW, label.getCentreY(), juce::jmax (0, label.getWidth() - textW), 1);
        }
    }
};

//==============================================================================
//  Milestone 8: a fixed-size grid of slot buttons for a VoiceBank (Pads,
//  Milestone 8; FX, Milestone 9 -- structurally identical panels, per PRD
//  §9's own "structurally identical to Pads"). Callback-based rather than
//  templated on VoiceBank<N> directly, so this UI component doesn't need to
//  know the engine type at all -- SessionComponent wires onSlotClicked to
//  whichever VoiceBank (padBank or fxBank) this panel represents.
//==============================================================================
//  UI_SPEC_PERFORM.md §2.5 (build-order step 5). Rebuilt on VoiceSlotCell
//  (DeckTriggerButton's plain-text slots are gone from here -- nothing else
//  referenced that class, so it's removed entirely, not left as dead code).
//
//  Page selector (1 2 3): Phase 1.1 P6 "Pads currently too small. Instead:
//  display 4 Pads per page, with 3 pages (total 12 pads). Same approach for
//  FX." The engine's VoiceBank<12> (untouched, off-limits this pass) is
//  already a real fixed 12-slot bank -- pages 1/2/3 were previously all
//  crammed into view at once (a cramped 4x3 grid, "too small" per the
//  brief), with the page selector wired but honestly inert (toasting "only
//  one bank" on 2/3) because nothing paged anything. Now each page shows
//  ONLY its own 4 of the 12 REAL slots (page 1 = slots 0-3, page 2 = 4-7,
//  page 3 = 8-11) -- no fabricated second bank, no engine change: the bank
//  already had 12 slots, this just stops showing all of them at once. Pages
//  2/3 are real now, not inert -- the old toast is gone.
class VoiceBankPanel : public juce::Component
{
public:
    static constexpr int kSlotsPerPage = 4;

    VoiceBankPanel (const juce::String& title, int numSlots)
    {
        titleLabel.setText (title, juce::dontSendNotification);
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        // SPEC_PERFORM_V2 GROUP F (105): same kerning treatment as every
        // other uppercase panel title (PADS/FX) -- see DockHeaderBar's own.
        titleLabel.setFont (performfonts::headingFont (11.0f).withExtraKerningFactor (0.01f));
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6cc));   // kTextDim
        addAndMakeVisible (titleLabel);

        for (int p = 0; p < 3; ++p)
        {
            auto* b = new juce::TextButton (juce::String (p + 1));
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (p == 0 ? 0xff7c5cff : 0xff151527));   // kIndigo : kCard
            b->setColour (juce::TextButton::textColourOffId, juce::Colour (0xffa3a6cc));   // kTextDim
            b->setLookAndFeel (&tactileButtonLookAndFeel());   // owner touch pass: same tactile face as the rest of the chrome
            b->onClick = [this, p]
            {
                activePage = p;
                for (int i = 0; i < pageButtons.size(); ++i)
                    pageButtons[i]->setColour (juce::TextButton::buttonColourId,
                                               juce::Colour (i == activePage ? 0xff7c5cff : 0xff151527));
                resized();   // Phase 1.1 P6: switching pages now really does change which 4 slots are laid out/visible
                if (onPageSelected != nullptr) onPageSelected (p);
            };
            addAndMakeVisible (b);
            pageButtons.add (b);
        }

        for (int i = 0; i < numSlots; ++i)
        {
            auto* cell = new VoiceSlotCell();
            cell->setIndex (i + 1);
            cell->onTap  = [this, i] { if (onSlotClicked != nullptr) onSlotClicked (i); };
            cell->onMenu = [this, i] { if (onSlotRightClicked != nullptr) onSlotRightClicked (i); };
            cell->onLibraryTap = [this, i] { if (onSlotLibraryTap != nullptr) onSlotLibraryTap (i); };
            cell->onDoubleTap = [this, i] { if (onSlotDoubleClicked != nullptr) onSlotDoubleClicked (i); };
            cell->onAssetDropped = [this, i] (const juce::String& assetId) { if (onSlotAssetDropped != nullptr) onSlotAssetDropped (i, assetId); };
            cell->onFilesDropped = [this, i] (const juce::StringArray& files) { if (onSlotFilesDropped != nullptr) onSlotFilesDropped (i, files); };
            addAndMakeVisible (cell);
            slotCells.add (cell);
        }

        // (The pack-import "+" button is gone -- owner: "I don't need the +
        // by the pads"; drag-and-drop onto the slots replaces it.)
    }

    std::function<void (int)> onSlotClicked;
    // SPEC_PERFORM_V2 GROUP F (103c): right-click now opens the same
    // VoiceEditorContent onDoubleTap below does (the popup menu this used
    // to open, showVoiceSlotMenu(), is gone -- task #98).
    std::function<void (int)> onSlotRightClicked;
    std::function<void (int)> onPageSelected;   // 0/1/2 -- see this class's own header comment
    std::function<void (int)> onSlotLibraryTap;   // SPEC_PERFORM_V2 GROUP H2 -- see VoiceSlotCell::onLibraryTap
    std::function<void (int)> onSlotDoubleClicked;   // SPEC_PERFORM_V2 GROUP F (103c) -- opens VoiceEditorContent
    std::function<void (int, const juce::String&)>      onSlotAssetDropped;   // Library card dropped on slot i
    std::function<void (int, const juce::StringArray&)> onSlotFilesDropped;   // OS files dropped on slot i

    // loadVoiceClip() (SessionComponent) calls these once per slot at load
    // time -- name/duration/bound-key text and empty-vs-loaded state, not
    // per-frame. setSlotActive() below is the ONLY one driven by the 15Hz timer.
    void setSlotEmpty (int i)
    {
        slotLoaded[(size_t) i] = false;
        slotCells[i]->setState (VoiceSlotCell::State::empty);
        slotCells[i]->setClipName ({});
    }
    void setSlotLoaded (int i, const juce::String& name, const juce::String& durationText)
    {
        slotLoaded[(size_t) i] = true;
        slotCells[i]->setClipName (name);
        slotCells[i]->setDurationText (durationText);
        slotCells[i]->setState (VoiceSlotCell::State::loaded);
    }
    void setSlotBoundKey (int i, const juce::String& key) { slotCells[i]->setBoundKey (key); }
    // SPEC_PERFORM_V2 GROUP F (103c): re-labels a slot WITHOUT touching its
    // loaded/empty state -- used to apply a name override on top of the
    // load-time filename-derived name, from VoiceEditorContent's rename.
    void setSlotDisplayName (int i, const juce::String& name) { if (slotLoaded[(size_t) i]) slotCells[i]->setClipName (name); }
    void setSlotAccentColour (int i, bool set, juce::Colour c) { slotCells[i]->setAccentColour (set, c); }

    // Called from the 15Hz UI timer -- isActive() is safe to read from the
    // message thread (OneShotVoice's own atomic-gate design). Only touches
    // loaded slots' loaded<->playing state; an empty slot stays empty
    // regardless (there's nothing to play). slotLoaded is this panel's own
    // bookkeeping (owner tracks what it needs, matching DeckCard/
    // DeckTriggerCell's push-only philosophy -- VoiceSlotCell exposes no
    // state getter).
    void setSlotActive (int i, bool active)
    {
        if (! slotLoaded[(size_t) i]) return;
        slotCells[i]->setState (active ? VoiceSlotCell::State::playing : VoiceSlotCell::State::loaded);
    }

    // Owner: playing-pad light animation -- one phase for the whole bank,
    // pushed from the same 15Hz timer as setSlotActive() above. Cells
    // repaint only while playing (see VoiceSlotCell::setPulsePhase).
    void setPulsePhase (double phase01)
    {
        for (auto* c : slotCells) c->setPulsePhase (phase01);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        // Owner touch pass ("the 1/2/3 by the pads is not touch friendly"):
        // the header row grew 18 -> 30px and the page buttons 32 -> 46px
        // wide, so each is a real ~28x46 tactile target instead of a
        // 14px-tall sliver. The extra height comes from the strip itself
        // (voiceBankArea grew to match in SessionComponent::resized()), not
        // from the pad cells.
        auto headerRow = area.removeFromTop (30);
        titleLabel.setBounds (headerRow.removeFromLeft (60));
        for (int p = 2; p >= 0; --p)
            pageButtons[p]->setBounds (headerRow.removeFromRight (46).reduced (2, 1));

        area.removeFromTop (4);

        // Phase 1.1 P6: only this page's own kSlotsPerPage (4) cells are
        // visible/positioned -- every other cell hides. One row of 4,
        // each getting the FULL remaining height (this is the direct fix
        // for "Pads currently too small": the same area 12 cramped cells
        // used to share is now shared by only 4).
        const int gap = 6;
        const int cellW = (area.getWidth() - (kSlotsPerPage - 1) * gap) / kSlotsPerPage;
        const int pageStart = activePage * kSlotsPerPage;
        for (int i = 0; i < slotCells.size(); ++i)
        {
            const bool onThisPage = i >= pageStart && i < pageStart + kSlotsPerPage;
            slotCells[i]->setVisible (onThisPage);
            if (! onThisPage) continue;
            const int col = i - pageStart;
            slotCells[i]->setBounds (area.getX() + col * (cellW + gap), area.getY(), cellW, area.getHeight());
        }
    }

private:
    juce::Label titleLabel;
    juce::OwnedArray<juce::TextButton> pageButtons;
    int activePage { 0 };
    juce::OwnedArray<VoiceSlotCell> slotCells;
    std::array<bool, 12> slotLoaded {};
};

//==============================================================================
//  Milestone 11: a scene slot button -- hold (~650ms) to save, tap to recall,
//  right-click for the context menu (PRD §11's own interaction model,
//  matching PRD §16's "long-press-to-save, tap-to-recall... used specifically
//  for scenes, to prevent an accidental tap from overwriting a saved
//  snapshot").
//
//  UI_SPEC_PERFORM.md §2.2 (build-order step 4): rebuilt from a plain
//  TextButton into a custom-painted Component for the spec's four distinct
//  states (empty/filled/active/arming) -- restyle only, the hold-to-
//  save/tap-to-recall/right-click GESTURE handling below is the exact same
//  logic that was already here, just moved off juce::Button's mouseDown/Up
//  onto juce::Component's (same override names, same semantics).
//
//  Arming's "border animates kQueued, filling clockwise over the 650ms
//  hold": this needs the hold-timer to tick DURING the hold, not just once
//  at the end, so the interval changed from a single 650ms one-shot to a
//  33ms repeat with elapsed time tracked explicitly -- still this
//  component's own pre-existing per-instance Timer (established since
//  Milestone 11), not a new category of timer the spec's own "reuse the
//  15Hz timer" note is about (that note is specifically about NOT giving
//  DeckCard/DeckTriggerCell their own timers; this one already had one).
//==============================================================================

//==============================================================================
//  UI_SPEC_PERFORM.md §2.2 (build-order step 4) -- the transport's BPM
//  readout: value 22px bold, "BPM" 9px kTextFaint beneath, tap opens numeric
//  entry (the owner supplies onTap; the actual AlertWindow lives in
//  SessionComponent, matching promptSetDeckTempo's own established pattern
//  -- this component only displays and forwards the gesture).
//==============================================================================
class BpmReadout : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onTap;

    void setBpm (double b) { bpm = b; repaint(); }

    // Phase 1.1 P7 "BPM pulse": gated on the SAME transportRunning-driven
    // liveBeatPhase every DeckCard/SceneButton already pulses from (see
    // SessionComponent::timerCallback()) -- no new phase source. setPulsing
    // resets brightness to full and stops repainting on every tick once
    // playback stops, matching SceneButton::setActiveState()'s own
    // "inactive never pulses" precedent.
    void setPulsing (bool on) { if (pulsing != on) { pulsing = on; if (! on) pulsePhase = 0.0; repaint(); } }
    void setPulsePhase (double phase01) { if (pulsing) { pulsePhase = juce::jlimit (0.0, 1.0, phase01); repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (juce::Colour (0xff151527));   // kCard
        g.fillRoundedRectangle (bounds, 8.0f);

        // Same "fast attack, slow decay" ease DeckCard::livePulseBrightness()
        // uses, but a narrower range (0.85-1.0) -- this is a numeric
        // readout, not a stage light; the pulse should read as a subtle
        // "in time with the beat" cue, not dim the number.
        float brightness = 1.0f;
        if (pulsing)
        {
            const float t = (float) pulsePhase;
            const float eased = 1.0f - (t * t);
            brightness = juce::jlimit (0.85f, 1.0f, 0.90f + 0.10f * eased);
        }

        auto area = getLocalBounds().reduced (4, 2);
        g.setColour (juce::Colour (0xfff2f0ff).withMultipliedBrightness (brightness));   // kTextBright, eased by pulse
        // Bug report visual-polish pass (LoopLab reference): numeric
        // readouts use a distinct monospace face against the UI's sans-serif
        // everywhere else -- reads as "data," not decoration, and digits
        // never shift width as the value changes. getDefaultMonospacedFontName()
        // resolves to whatever the OS actually has (Consolas on Windows) --
        // no new font asset to embed for this.
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 22.0f, juce::Font::bold)));
        g.drawText (juce::String (bpm, 1), area.removeFromTop (area.getHeight() - 12), juce::Justification::centred, false);

        g.setColour (juce::Colour (0xff6f7099));   // kTextFaint
        // SPEC_PERFORM_V2 GROUP F (105): same kerning treatment as every
        // other uppercase caption -- see SIGNATURE's own (its identical
        // "dim caption under a bold readout" sibling).
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::plain)).withExtraKerningFactor (0.12f));
        g.drawText ("BPM", area, juce::Justification::centred, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()) && onTap != nullptr) onTap();
    }

private:
    double bpm { 120.0 };
    bool pulsing { false };
    double pulsePhase { 0.0 };
};

class SceneButton : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit SceneButton (int slotNumberIn) : slotNumber (slotNumberIn) {}

    std::function<void()> onTap;
    std::function<void()> onHoldComplete;
    std::function<void()> onRightClick;

    void setScene (bool filledIn, const juce::String& nameIn)
    {
        filled = filledIn;
        name = nameIn;
        // Owner direction: "show the colours instead of the names" -- the
        // button face is colour-first now, so the scene's name lives here
        // in the tooltip instead of painted on the face.
        setTooltip ((name.isNotEmpty() ? name + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) : juce::String())
                     + juce::String (juce::CharPointer_UTF8 ("Tap to recall \xc2\xb7 hold to save \xc2\xb7 right-click to recolour")));
        repaint();
    }

    void setActiveState (bool a) { if (active != a) { active = a; if (! a) pulsePhase = 0.0; repaint(); } }

    // SPEC_PERFORM_V2 GROUP E: owner-assignable colour -- defaults to the
    // existing kIndigo look (every scene button already had) until the
    // owner sets one via right-click.
    void setAccentColour (juce::Colour c) { if (accent != c) { accent = c; repaint(); } }

    // Phase 1.1 P1 "Scene Buttons... pulse during playback": 0..1 within the
    // current beat, pushed from SessionComponent's existing 15Hz timer using
    // the SAME liveBeatPhase every DeckCard/DeckTriggerCell already reads --
    // no new timer, no free-running animation of its own. Ignored (and left
    // at 0, i.e. no pulse) unless active -- an inactive scene never pulses.
    void setPulsePhase (double phase01) { if (active) { pulsePhase = juce::jlimit (0.0, 1.0, phase01); repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = 8.0f;

        const juce::Colour kIndigoDim (0xff3a2f6b);
        const juce::Colour kBorder    (0xff2b2b4d);
        const juce::Colour kQueued    (0xffffc933);
        const juce::Colour kTextFaint (0xff6f7099);
        const juce::Colour kTextBright(0xfff2f0ff);

        // SPEC_PERFORM_V2 GROUP E "Scenes light up like a MIDI keyboard":
        // every kIndigo below is now the owner-assignable `accent` (defaults
        // to the same kIndigo look every scene button already had) -- one
        // colour drives both the glow and the fill/border, so "lit in that
        // colour" reads as one coherent light, not a tinted fill under a
        // fixed-colour glow.
        //
        // (The old "glow when active" block that drew OUTSIDE the button's
        // bounds is gone -- juce::Graphics clips painting to the component,
        // so it was invisible in practice, the same bug DeckCard's ring
        // had. The visible replacement is the inner light-up in the filled
        // branch below.)

        if (! filled)
        {
            g.setColour (kIndigoDim.withAlpha (0.40f));
            g.fillRoundedRectangle (bounds, radius);
            juce::Path outline, dashed;
            outline.addRoundedRectangle (bounds, radius);
            const float dashes[] { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (kBorder);
            g.strokePath (dashed, juce::PathStrokeType (1.0f));

            g.setColour (kTextFaint);
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::plain)));
            g.drawText (juce::String (slotNumber), getLocalBounds(), juce::Justification::centred, false);
        }
        else
        {
            // "pulse during playback": the same eased brightness curve
            // DeckCard::livePulseBrightness() uses, applied to the active
            // fill instead of a border -- one beat-synced idiom, reused.
            const float t = (float) pulsePhase;
            const float eased = 1.0f - (t * t);
            const float pulseBrightness = juce::jlimit (0.80f, 1.0f, 0.80f + 0.20f * eased);

            // Owner direction: "the scene colours should be ON... show the
            // colours instead of the names." A filled scene shows its
            // assigned colour even while idle (dimmed, like a loaded MIDI
            // pad); the active scene is the same colour at full brightness.
            if (active)
                g.setColour (accent.brighter (pressed ? 0.3f : 0.0f).withMultipliedBrightness (pulseBrightness));
            else
                g.setColour (accent.withMultipliedBrightness (0.40f).withMultipliedSaturation (0.85f));
            g.fillRoundedRectangle (bounds, radius);
            g.setColour (active ? accent : accent.withMultipliedBrightness (0.65f));
            g.drawRoundedRectangle (bounds, radius, active ? 2.0f : 1.0f);

            // Owner direction "light (motion) up when engaged and playing":
            // a visible inner light-up -- concentric white-hot strokes
            // breathing with the SAME beat pulse as the fill. Drawn INSIDE
            // the bounds (an outward glow is clipped by the component's own
            // paint area -- the exact bug the removed block above had).
            if (active)
            {
                for (int i = 3; i >= 1; --i)
                {
                    const float inset = (float) i * 1.5f;
                    g.setColour (juce::Colours::white.withAlpha (0.12f * pulseBrightness / (float) i));
                    g.drawRoundedRectangle (bounds.reduced (inset), juce::jmax (2.0f, radius - inset * 0.5f), 2.0f);
                }
            }

            // Slot number only -- the name moved to the tooltip (see
            // setScene()); the colour itself is the identity now.
            g.setColour (active ? kTextBright : kTextBright.withAlpha (0.75f));
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (juce::String (slotNumber), getLocalBounds(), juce::Justification::centred, false);
        }

        // "animate on press": immediate visual feedback while the mouse is
        // down (a slight inset + brighter fill above) -- deliberately NOT an
        // interpolated tween (nothing here has a continuous per-frame
        // animation clock; see this class's own header comment on why it
        // doesn't gain one just for this), but still a real, instant state
        // change on press/release rather than a static button.
        if (pressed)
        {
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRoundedRectangle (bounds.reduced (1.5f), radius);
        }

        // Arming: a clockwise-filling kQueued ring, drawn ON TOP of whichever
        // base state above -- only while the hold timer is actually running.
        if (isTimerRunning())
        {
            juce::Path arc;
            const float startAngle = -juce::MathConstants<float>::halfPi;
            const float sweep = (float) armProgress * juce::MathConstants<float>::twoPi;
            arc.addArc (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
                        startAngle, startAngle + sweep, true);
            g.setColour (kQueued);
            g.strokePath (arc, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown()) { if (onRightClick != nullptr) onRightClick(); return; }
        held = false;
        pressed = true;
        armProgress = 0.0;
        holdStartMs = juce::Time::getMillisecondCounterHiRes();
        startTimer (33);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        stopTimer();
        armProgress = 0.0;
        pressed = false;
        repaint();
        if (! held && ! e.mods.isRightButtonDown() && getLocalBounds().contains (e.getPosition()) && onTap != nullptr) onTap();
    }

private:
    void timerCallback() override
    {
        const double elapsed = juce::Time::getMillisecondCounterHiRes() - holdStartMs;
        if (elapsed >= 650.0)
        {
            stopTimer();
            held = true;
            armProgress = 0.0;
            repaint();
            if (onHoldComplete != nullptr) onHoldComplete();
            return;
        }
        armProgress = elapsed / 650.0;
        repaint();
    }

    int slotNumber { 1 };
    bool filled { false };
    bool active { false };
    bool held { false };
    bool pressed { false };
    double pulsePhase { 0.0 };
    juce::String name;
    double holdStartMs { 0.0 };
    double armProgress { 0.0 };
    juce::Colour accent { 0xff7c5cff };   // SPEC_PERFORM_V2 GROUP E: owner-assignable, defaults to the prior fixed kIndigo

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneButton)
};

//==============================================================================
//  Phase 1.1 P1 "Time Signature Buttons: same treatment [as Scene Buttons] --
//  selected signature should illuminate." Replaces the signature rail's old
//  plain juce::TextButton array (a buttonColourId swap was the entire prior
//  "highlight," done via SessionComponent's own 15Hz timer). Deliberately a
//  new, smaller class rather than reusing SceneButton directly -- a
//  signature has no scene name/fill state/hold-to-arm-and-overwrite gesture,
//  just "which one is selected," so SceneButton's extra fields would be dead
//  weight here. Same glow-approximation and instant-press-feedback idioms,
//  same reasoning, applied to a simpler shape.
//==============================================================================
class SignatureRailButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    // Explicit, even though it does nothing extra: JUCE_DECLARE_NON_COPYABLE
    // below deletes the copy constructor, which counts as a user-declared
    // constructor and suppresses the implicit default one too -- same reason
    // DeckCard's own constructor isn't just `= default`.
    SignatureRailButton() = default;

    std::function<void()> onTap;
    // SPEC_PERFORM_V2 GROUP E: same right-click-to-colour gesture as
    // SceneButton's own onRightClick.
    std::function<void()> onRightClick;

    void setLabel (const juce::String& l) { label = l; repaint(); }
    void setSelected (bool s) { if (selected != s) { selected = s; repaint(); } }

    // SPEC_PERFORM_V2 GROUP E: owner-assignable colour -- defaults to the
    // existing kIndigo look every signature button already had.
    void setAccentColour (juce::Colour c) { if (accent != c) { accent = c; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = 8.0f;

        const juce::Colour kIndigoDim  (0xff3a2f6b);
        const juce::Colour kTextBright (0xfff2f0ff);
        const juce::Colour kTextFaint  (0xffa3a6cc);

        // "illuminate" -- same layered-alpha fake-glow SceneButton's own
        // comment explains (JUCE has no cheap real blur). Uses the owner-
        // assignable `accent` now, same reasoning as SceneButton's own.
        if (selected)
        {
            for (int i = 3; i >= 1; --i)
            {
                const float expand = (float) i * 2.0f;
                g.setColour (accent.withAlpha (0.10f / (float) i));
                g.fillRoundedRectangle (bounds.expanded (expand), radius + expand);
            }
        }

        g.setColour (selected ? accent : kIndigoDim);
        g.fillRoundedRectangle (bounds, radius);

        if (pressed)
        {
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRoundedRectangle (bounds.reduced (1.5f), radius);
        }

        g.setColour (selected ? kTextBright : kTextFaint);
        g.setFont (juce::Font (juce::FontOptions (13.0f, selected ? juce::Font::bold : juce::Font::plain)));
        g.drawText (label, getLocalBounds(), juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick != nullptr) onRightClick(); return; }
        pressed = true;
        repaint();
        touchHold.onLongPress = [this] (juce::Point<int>) { pressed = false; repaint(); if (onRightClick != nullptr) onRightClick(); };
        touchHold.begin (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override { touchHold.drag (e); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        pressed = false;
        repaint();
        if (touchHold.end()) return;
        if (getLocalBounds().contains (e.getPosition()) && onTap != nullptr) onTap();
    }

private:
    juce::String label;
    bool selected { false };
    bool pressed  { false };
    juce::Colour accent { 0xff7c5cff };   // SPEC_PERFORM_V2 GROUP E: owner-assignable, defaults to the prior fixed kIndigo
    eztouch::LongPress touchHold;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignatureRailButton)
};

//==============================================================================
//  Phase 1.1 Priority 2 "Library: open docked at ~50% height by default (not
//  full-screen), allow maximize/restore/resize" -- the drag-to-resize strip
//  along a docked panel's own top edge. A plain "click-drag changes a
//  height" gesture, same idiom as any OS's own window-splitter, just owner-
//  drawn since JUCE has no built-in docking widget. Deliberately generic
//  (title only, no Library-specific knowledge) -- Priority 3's Mixer bullet
//  explicitly asks for "the same treatment as Library," so this is written
//  to be reused there without modification, not copy-pasted.
//==============================================================================
class DockHeaderBar : public juce::Component
{
public:
    // Explicit, even though it does nothing extra: JUCE_DECLARE_NON_COPYABLE
    // below deletes the copy constructor, which counts as a user-declared
    // constructor and suppresses the implicit default one too -- same reason
    // DeckCard/SceneButton/SignatureRailButton's own constructors aren't
    // just `= default` either.
    DockHeaderBar() = default;

    std::function<void (int deltaY)> onDrag;   // dragged this many px vertically since the last event

    void setTitle (const juce::String& t) { title = t; repaint(); }

    void paint (juce::Graphics& g) override
    {
        // Raw hex literals, not performlive::kXxx -- this class is defined
        // BEFORE the performlive namespace exists in this file (same
        // constraint DeckTriggerCell/DeckCard/etc. above already work
        // around); == performlive::kCard/kBorder/kTextFaint/kTextDim.
        const juce::Colour kCard      (0xff151527);
        const juce::Colour kBorder    (0xff2b2b4d);
        const juce::Colour kTextFaint (0xff6f7099);
        const juce::Colour kTextDim   (0xffa3a6cc);

        g.setColour (kCard);
        g.fillRect (getLocalBounds());
        g.setColour (kBorder);
        g.fillRect (getLocalBounds().removeFromBottom (1));

        // grip affordance -- three short centred lines, the same visual
        // shorthand for "drag me" used by native OS window splitters.
        g.setColour (kTextFaint);
        const int cx = getWidth() / 2;
        for (int i = 0; i < 3; ++i)
            g.fillRect (cx - 14, 4 + i * 3, 28, 1);

        auto area = getLocalBounds().reduced (12, 0);
        area.removeFromTop (10);
        // SPEC_PERFORM_V2 GROUP F (105): same extra-kerning treatment the
        // header's own "LOGO" wordmark uses for bold uppercase chrome --
        // applied here for consistency across every dock/panel title
        // (LIBRARY/MIXER/EDITOR — see DockHeaderBar::setTitle() callers).
        // Font stack: headings are Space Grotesk (performfonts::headingFont).
        g.setFont (performfonts::headingFont (11.0f).withExtraKerningFactor (0.01f));
        g.setColour (kTextDim);
        g.drawText (title, area, juce::Justification::centredLeft, false);
    }

    void mouseDown (const juce::MouseEvent& e) override { lastScreenY = e.getScreenPosition().y; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int y = e.getScreenPosition().y;
        const int dy = y - lastScreenY;
        lastScreenY = y;
        if (onDrag != nullptr) onDrag (dy);
    }

private:
    juce::String title;
    int lastScreenY { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DockHeaderBar)
};

//==============================================================================
//  Milestone 11 (generalized in Milestone 15): a minimal, reusable transient
//  notification -- PRD §11's own "explicitly surfaced to the user via a
//  toast suffix, not silent" requirement for scene recall's tempo-lock-
//  override case, and PRD §16's cross-cutting "toasts confirm every state
//  change in plain language." Built once here rather than as a one-off for
//  scene recall, since Milestone 15's own job is applying this same pattern
//  everywhere else.
//==============================================================================
class ToastOverlay : public juce::Component, private juce::Timer
{
public:
    ToastOverlay() { setInterceptsMouseClicks (false, false); }

    void show (const juce::String& text, int durationMs = 2500)
    {
        message = text;
        setVisible (true);
        toFront (false);
        repaint();
        startTimer (durationMs);
    }

    void paint (juce::Graphics& g) override
    {
        if (message.isEmpty()) return;
        auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xe0202030));
        g.fillRoundedRectangle (area, 8.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (15.0f)));
        g.drawFittedText (message, getLocalBounds().reduced (12, 6), juce::Justification::centred, 3);
    }

private:
    void timerCallback() override
    {
        stopTimer();
        message.clear();
        setVisible (false);
    }

    juce::String message;
};

//==============================================================================
//  Pure, JUCE-independent tempo-control helpers (Milestone 4, M4-T5/M4-T6).
//  Kept free of any class state so they can be lifted verbatim into a
//  standalone diagnostic and verified without interactive UI input, the same
//  disclosure pattern used throughout Milestone 3 for UI-adjacent logic in
//  this JUCE-only file.
//==============================================================================

// Tap tempo: given the full history of tap timestamps (milliseconds, any
// monotonically-increasing time base), walks backward from the most recent
// tap accumulating inter-tap intervals until either a gap exceeds
// resetThresholdMs (a pause -- stop, don't blend in stale history) or
// maxIntervals is reached, then returns the BPM implied by their average.
// Returns 0.0 if fewer than two taps are available in the current run
// (nothing to average yet) -- callers should treat 0.0 as "no update".
inline double computeTapTempo (const std::vector<double>& tapTimestampsMs,
                                double resetThresholdMs = 2000.0,
                                size_t maxIntervals = 8)
{
    if (tapTimestampsMs.size() < 2) return 0.0;

    std::vector<double> intervals;
    for (size_t i = tapTimestampsMs.size() - 1; i > 0; --i)
    {
        const double gap = tapTimestampsMs[i] - tapTimestampsMs[i - 1];
        if (gap <= 0.0 || gap > resetThresholdMs) break;
        intervals.push_back (gap);
        if (intervals.size() >= maxIntervals) break;
    }
    if (intervals.empty()) return 0.0;

    double sum = 0.0;
    for (double v : intervals) sum += v;
    const double avgMs = sum / (double) intervals.size();
    return avgMs > 0.0 ? 60000.0 / avgMs : 0.0;
}

// Resolves one deck's effective target tempo (PRODUCT_REQUIREMENTS.md §5):
// an explicit per-deck override always wins, regardless of LOCK. With no
// override, LOCK on means "warp to master tempo"; LOCK off means "play as
// recorded" -- no forced target at all, represented as std::nullopt rather
// than any particular BPM value, so a caller can't mistake "no warp" for
// "warp to some sentinel tempo".
inline std::optional<double> resolveEffectiveTempo (const std::optional<double>& deckOverride,
                                                     bool lockEnabled,
                                                     double masterBpm)
{
    if (deckOverride.has_value()) return deckOverride;
    if (lockEnabled) return masterBpm;
    return std::nullopt;
}

//==============================================================================
//  Waveform view with region-end and fade in/out drag handles — milestone 3
//  (M3-T5 waveform, M3-T6 region-end handle, M3-T7 fade handles). Draws
//  per-pixel min/max peaks of the layer's sample data, so a loud section
//  visibly looks louder than a quiet one, plus a draggable amber line at the
//  loop-region boundary and two small amber markers at the top for fade
//  in/out (matching the reference's "amber handles at the top set the fade
//  in and out").
//
//  Three possible drag targets are disambiguated by where the mouse goes
//  down: inside the top strip (see topStripHeight), whichever of the fade-in/
//  fade-out markers is nearer; anywhere else, the region-end handle -- the
//  same, unchanged hit-test M3-T6 shipped (no X-proximity requirement there,
//  preserved as-is rather than tightened as part of this task).
//
//  No beat grid yet: drawing one needs each layer's own detected
//  tempo/transients, which today are only computed and kept for deck A /
//  layer 1 (see loadLayer) -- persisting them for every layer is a separate,
//  non-trivial change, left for a later refinement, not folded in silently.
//
//  Architectural limitations discovered and documented, not solved, per this
//  task's own instructions (also project/KNOWN_BUGS.md #13 and
//  project/MILESTONE_3_EXECUTION_PLAN.md M3-T6/M3-T7):
//
//  1. Deck.h's Layer has a region *length* but no region *start* -- a start
//     trim handle needs a new Deck.h field, out of this file's declared
//     scope (Main.cpp only). Carried over unchanged from M3-T6.
//     SPEC_M6_EDIT_WINDOW.md §B literally asks for a left/start handle too;
//     this is the exact honest gap that spec pre-authorizes flagging rather
//     than faking -- there is still only an END handle drawn below. Reported
//     to the owner in the M6 step-2 report, not silently invented.
//  2. regionLength, trimmed, fadeInSamples, and fadeOutSamples are plain
//     int/bool, not std::atomic<bool> like enabled (Deck.h:46). All four are
//     designed to be set once at load, before the audio thread exists, never
//     mutated during live playback. This view's handles (region-end since
//     M3-T6, now also fade in/out) write these fields from the message
//     thread while the audio thread may concurrently read them via
//     loopLength()/render() if this layer's deck happens to be playing -- a
//     genuine, reachable data race, not theoretical. Properly fixing this
//     needs a Deck.h change (making these fields atomic), out of scope here.
//
//  Holds a reference, not a copy, to the live Layer -- valid for as long as
//  the editor is open, since the Layer is owned by the long-lived
//  Session/Deck, not by this transient dialog.
//
//  SPEC_M6_EDIT_WINDOW.md §B (step 2): restyle only -- accentColour/
//  setBarGrid() are new presentation inputs, and hit-testing now requires
//  being within handleGrabPixels of the actual grip (previously ANY drag
//  below topStripHeight moved the region-end handle, regardless of where it
//  started -- a precision bug the spec's "44px hit areas" language exposed).
//  The commit targets themselves (layer.regionLength/fadeInSamples/
//  fadeOutSamples, previewPosition) are byte-for-byte the same fields, same
//  commit-on-release timing, as before this step -- no editing feature was
//  added, removed, or rewired. Defined ahead of the `performlive` namespace
//  in this file (like every other early class), so it uses raw hex literals
//  that match performlive::kWorkspaceBg/kQueued/kGridLine/kGridLineHi
//  exactly, annotated inline -- same convention MixerChannelStrip already
//  established.
//==============================================================================
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (ezdeck::Layer& layerRef) : layer (layerRef) {}

    // SPEC_M6_EDIT_WINDOW.md §B: the layer's identity colour, matching the
    // lane it came from (columnAccent(l)) -- defaults to the pre-M6 green so
    // any caller that never sets one (there is none left, but this keeps the
    // class usable stand-alone) still renders sensibly.
    void setAccentColour (juce::Colour c) { accentColour = c; repaint(); }

    // SPEC_M6_EDIT_WINDOW.md §B: "a bar grid overlaid faintly (reuse the
    // arrangement ruler's bar math)". Same 4/4-at-the-tagged-tempo formula
    // seekStemEditorTo()/the ruler already use. bpm<=0 or sampleRateHz<=0
    // (no context given yet) leaves the grid simply undrawn.
    void setBarGrid (double bpm, double sampleRateHz)
    {
        samplesPerBar = (bpm > 0.0 && sampleRateHz > 0.0) ? (60.0 / bpm) * 4.0 * sampleRateHz : 0.0;
        repaint();
    }

    // Owner: "the window should be a grid 1/4 1/2 1/16 1/8 1/32 etc... like a
    // standard DAW audio editing window." divisionsPerBar counts the grid
    // lines in ONE bar: 1 = bar lines only, 4 = 1/4 notes, 8 = 1/8, and so on
    // (4/4 assumed, matching samplesPerBar's own formula above). 0 = grid off.
    void setGridDivisionsPerBar (int divisions)
    {
        gridDivisionsPerBar = juce::jmax (0, divisions);
        repaint();
    }

    int  getGridDivisionsPerBar() const { return gridDivisionsPerBar; }

    // Samples per grid line at the current division -- the snap quantum every
    // snapping tool should use, so the grid you SEE is the grid you snap to.
    // 0 when there is no usable grid.
    double gridStepSamples() const
    {
        if (samplesPerBar <= 0.0 || gridDivisionsPerBar <= 0) return 0.0;
        return samplesPerBar / (double) gridDivisionsPerBar;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff07070f));   // == performlive::kWorkspaceBg
        const auto& data = layer.left;
        if (data.empty()) return;

        const int   w    = getWidth();
        const int   h    = getHeight();
        const float midY = (float) h * 0.5f;

        // Owner: "like a standard DAW audio editing window" -- a real
        // hierarchical grid rather than the old bar-lines-only pass. Every
        // grid line at the chosen division is drawn, and its WEIGHT tells you
        // what it is, exactly as a DAW's ruler does:
        //     bar line   -- brightest, full height, 1.5px
        //     beat line  -- mid, full height, 1px
        //     subdivision (1/8, 1/16, 1/32...) -- faint, and inset from the
        //                   top/bottom edges so the bar/beat structure still
        //                   reads at a glance instead of becoming a wall
        // Subdivision lines are skipped automatically when they would be
        // closer than ~4px (zoomed out), which is what keeps a 1/32 grid on a
        // long clip legible instead of solid grey.
        if (samplesPerBar > 0.0 && gridDivisionsPerBar > 0)
        {
            const double stepSamples = samplesPerBar / (double) gridDivisionsPerBar;
            // one beat = a quarter note = samplesPerBar/4 (4/4, matching
            // setBarGrid's own formula)
            const double beatSamples  = samplesPerBar * 0.25;

            // Owner: "I should be able to adjust the audio clip to put it on
            // the grid well if I want to." The grid's origin is the clip's
            // own START MARKER, not sample 0 -- so dragging the start marker
            // onto the first downbeat is what aligns the audio to the grid,
            // and bar 1 beat 1 always lands exactly where the loop begins.
            // (Aligning by moving the grid rather than the samples also keeps
            // the audio itself untouched.)
            const double gridOrigin = (double) layer.regionStartClamped();
            const int    totalSteps = (int) (((double) layer.numFrames() - gridOrigin) / stepSamples) + 1;

            const float pixelsPerStep = pixelForSample ((int) stepSamples) - pixelForSample (0);
            const bool  drawSubs      = pixelsPerStep >= 4.0f;

            for (int i = 0; i < totalSteps; ++i)
            {
                const double fromOrigin = (double) i * stepSamples;
                const double sample     = gridOrigin + fromOrigin;
                const float  x          = pixelForSample ((int) sample);
                if (x < -2.0f) continue;
                if (x > (float) w + 2.0f) break;

                // classify by what this line lands on, not by loop index --
                // works for every division without special cases. Measured
                // from the grid ORIGIN, so bar/beat weighting stays correct
                // however far the start marker has been moved.
                const double posInBar  = std::fmod (fromOrigin, samplesPerBar);
                const double posInBeat = std::fmod (fromOrigin, beatSamples);
                const bool isBarLine  = posInBar  < stepSamples * 0.5 || (samplesPerBar - posInBar)  < stepSamples * 0.5;
                const bool isBeatLine = posInBeat < stepSamples * 0.5 || (beatSamples  - posInBeat) < stepSamples * 0.5;

                if (isBarLine)
                {
                    g.setColour (juce::Colour (0xff474a68u));
                    g.drawLine (x, 0.0f, x, (float) h, 1.5f);
                }
                else if (isBeatLine)
                {
                    g.setColour (juce::Colour (0xff363850u));   // == kGridLineHi
                    g.drawLine (x, 0.0f, x, (float) h, 1.0f);
                }
                else if (drawSubs)
                {
                    g.setColour (juce::Colour (0xff23253au));
                    g.drawLine (x, (float) h * 0.12f, x, (float) h * 0.88f, 1.0f);
                }
            }
        }

        // Roadmap "zoom/pan": scan only the current [viewStartFraction,
        // viewStartFraction + 1/zoomFactor) window, not always the whole
        // buffer -- at the default zoomFactor=1/viewStartFraction=0 this
        // covers the same [0, data.size()) range as before.
        const int windowStart  = (int) (viewStartFraction * (double) data.size());
        const int windowLength = juce::jmax (1, (int) ((double) data.size() / (double) zoomFactor));
        const int samplesPerPixel = juce::jmax (1, windowLength / juce::jmax (1, w));
        g.setColour (accentColour);
        for (int x = 0; x < w; ++x)
        {
            const int start = windowStart + x * samplesPerPixel;
            if (start >= (int) data.size()) break;
            const int end = juce::jmin ((int) data.size(), start + samplesPerPixel);

            float mn = 0.0f, mx = 0.0f;
            for (int i = start; i < end; ++i)
            {
                const float v = data[(size_t) i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }

            // roadmap "gain overlay": the drawn peaks scale with the
            // layer's CURRENT gain, so raising/lowering it (slider or
            // Normalize) visibly grows/shrinks the waveform -- unclamped,
            // so a gain pushed toward clipping visibly draws past the
            // component's own height (clipped by JUCE like any other
            // off-bounds draw), a cheap visual clipping cue.
            mn *= layer.gain;
            mx *= layer.gain;

            const float y1 = midY - mx * midY * 0.95f;
            const float y2 = midY - mn * midY * 0.95f;
            g.drawLine ((float) x, y1, (float) x, juce::jmax (y2, y1 + 1.0f));
        }

        // Roadmap "zoom/pan": a small corner readout so zoom is discoverable
        // without a dedicated control -- only shown once actually zoomed in.
        if (zoomFactor > 1.01f)
        {
            g.setColour (juce::Colour (0xffa3a6ccu).withAlpha (0.85f));   // == performlive::kTextDim
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (juce::String (juce::roundToInt (zoomFactor * 100.0f)) + "%",
                        juce::Rectangle<int> (w - 50, h - 16, 46, 14), juce::Justification::centredRight, false);
        }

        // region-end handle: while dragging it, show the pending
        // (uncommitted) position; otherwise show the layer's actual region.
        // The dimmed rect to its right shows the excluded tail -- there is
        // no dimming on the left because there is no region-start to exclude
        // against (see this class's header comment, gap #1).
        const bool  draggingRegion = dragging && dragMoved && dragTarget == DragTarget::regionEnd;
        const bool  draggingStart  = dragging && dragMoved && dragTarget == DragTarget::regionStart;
        const int   shownStart     = (draggingStart && pendingRegionStart >= 0)
                                        ? pendingRegionStart : layer.regionStartClamped();
        const int   shownRegionLen = (draggingRegion && pendingRegionLength > 0)
                                        ? pendingRegionLength : layer.loopLength();
        const float startX  = pixelForSample (shownStart);
        const float handleX = pixelForSample (shownStart + shownRegionLen);

        // Everything OUTSIDE the region dims -- the excluded head on the left
        // (new: there is a region start now) and the excluded tail on the
        // right, so what actually plays is unmistakable.
        if (startX > 0.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, startX, (float) h));
        }
        if (handleX < (float) w)
        {
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (handleX, 0.0f, (float) w - handleX, (float) h));
        }

        g.setColour (juce::Colour (0xffffc933u));   // == performlive::kQueued -- established "editable handle" amber
        g.drawLine (handleX, 0.0f, handleX, (float) h, draggingRegion ? 3.0f : 2.0f);
        g.fillRoundedRectangle (handleX - 7.0f, 0.0f, 14.0f, 24.0f, 3.0f);   // visible grip, not hover-only

        // Owner's START marker -- same amber grip language as the end handle,
        // mirrored (grip sits to the right of its line so it stays on-screen
        // at sample 0).
        g.drawLine (startX, 0.0f, startX, (float) h, draggingStart ? 3.0f : 2.0f);
        g.fillRoundedRectangle (startX - 7.0f, 0.0f, 14.0f, 24.0f, 3.0f);

        // fade-in / fade-out ramps: filled triangles across the top strip so
        // the fade SHAPE reads visually, not just a point marker
        const bool draggingFadeIn  = dragging && dragMoved && dragTarget == DragTarget::fadeIn;
        const bool draggingFadeOut = dragging && dragMoved && dragTarget == DragTarget::fadeOut;
        const int  shownFadeIn     = (draggingFadeIn  && pendingFadeIn  >= 0) ? pendingFadeIn  : layer.fadeInSamples;
        const int  shownFadeOut    = (draggingFadeOut && pendingFadeOut >= 0) ? pendingFadeOut : layer.fadeOutSamples;

        // fades are measured from the REGION's own edges (Deck::render uses
        // posInRegion), so both handles move with the start marker
        const float fadeInX  = pixelForSample (shownStart + shownFadeIn);
        const float fadeOutX = pixelForSample (shownStart + shownRegionLen - shownFadeOut);

        g.setColour (juce::Colour (0xffffc933u).withAlpha (0.30f));
        juce::Path fadeInPath;
        fadeInPath.startNewSubPath (0.0f, topStripHeight);
        fadeInPath.lineTo (fadeInX, 0.0f);
        fadeInPath.lineTo (0.0f, 0.0f);
        fadeInPath.closeSubPath();
        g.fillPath (fadeInPath);

        juce::Path fadeOutPath;
        fadeOutPath.startNewSubPath ((float) w, topStripHeight);
        fadeOutPath.lineTo (fadeOutX, 0.0f);
        fadeOutPath.lineTo ((float) w, 0.0f);
        fadeOutPath.closeSubPath();
        g.fillPath (fadeOutPath);

        g.setColour (juce::Colour (0xffffc933u));
        g.fillEllipse (fadeInX  - 5.0f, topStripHeight - 10.0f, 10.0f, 10.0f);
        g.fillEllipse (fadeOutX - 5.0f, topStripHeight - 10.0f, 10.0f, 10.0f);

        // preview playhead (M3-T9) -- distinct colour/style from the amber
        // region-end/fade markers, since it's just a click position, not an
        // edit to the layer
        const float previewX = pixelForSample (previewPosition);
        g.setColour (juce::Colour (0xff67e8f9));
        g.drawLine (previewX, 0.0f, previewX, (float) h, 1.5f);
        g.fillEllipse (previewX - 3.0f, (float) h - 8.0f, 6.0f, 6.0f);

        // Owner: "I should see the playhead moving in the editor." A LIVE
        // position, pushed from the UI timer while this clip is actually
        // sounding (deck playback or editor preview) -- deliberately a
        // different colour and weight from the cyan click-marker above, which
        // is a static "where I clicked", not "where the audio is now".
        if (livePlayheadActive && livePlayheadSample >= 0)
        {
            const float liveX = pixelForSample (livePlayheadSample);
            if (liveX >= -2.0f && liveX <= (float) w + 2.0f)
            {
                g.setColour (juce::Colour (0xff2ee86a).withAlpha (0.35f));
                g.drawLine (liveX, 0.0f, liveX, (float) h, 4.0f);
                g.setColour (juce::Colour (0xffd7ffe6));
                g.drawLine (liveX, 0.0f, liveX, (float) h, 1.6f);
                juce::Path head;   // small downward triangle at the top, DAW-style
                head.addTriangle (liveX - 5.0f, 0.0f, liveX + 5.0f, 0.0f, liveX, 8.0f);
                g.fillPath (head);
            }
        }
    }

    // Owner's moving playhead -- pushed from ClipEditorContent's timer.
    // sample < 0 or active == false simply hides it.
    void setLivePlayhead (int sample, bool active)
    {
        if (livePlayheadSample == sample && livePlayheadActive == active) return;
        livePlayheadSample = sample;
        livePlayheadActive = active;
        repaint();
    }

    // Owner: touch-friendly zoom (Ctrl+wheel exists but isn't discoverable
    // and isn't reachable on a touchscreen). Zooms about the CENTRE of the
    // current view, so repeated taps converge where the user is looking.
    void zoomBy (float factor)
    {
        const double centreFrac = viewStartFraction + 0.5 / (double) zoomFactor;
        zoomFactor = juce::jlimit (1.0f, 64.0f, zoomFactor * factor);
        viewStartFraction = centreFrac - 0.5 / (double) zoomFactor;
        clampView();
        repaint();
        if (onViewChanged) onViewChanged();
    }

    void zoomToFit()
    {
        zoomFactor = 1.0f;
        viewStartFraction = 0.0;
        repaint();
        if (onViewChanged) onViewChanged();
    }

    // Centres the view on a sample without changing the zoom level -- used by
    // "go to start marker" / "go to end marker" so the user can inspect
    // exactly where a marker sits while zoomed right in.
    void centreOnSample (int sample)
    {
        const int total = layer.numFrames();
        if (total <= 0) return;
        const double frac = juce::jlimit (0.0, 1.0, (double) sample / (double) total);
        viewStartFraction = frac - 0.5 / (double) zoomFactor;
        clampView();
        repaint();
        if (onViewChanged) onViewChanged();
    }

    float  getZoomFactor() const { return zoomFactor; }

    // --- view range, for the owner's horizontal scrollbar ------------------
    double getViewStartFraction() const { return viewStartFraction; }
    double getVisibleFraction()   const { return 1.0 / (double) zoomFactor; }
    void   setViewStartFraction (double f) { viewStartFraction = f; clampView(); repaint(); }

    // Fired whenever the view range changes from INSIDE this component
    // (wheel zoom/pan), so the scrollbar can follow without polling.
    std::function<void()> onViewChanged;

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStartPos = e.position;
        dragMoved    = false;
        dragging     = true;

        const int   regionStart = layer.regionStartClamped();
        const int   regionLen   = layer.loopLength();
        const float regionX     = pixelForSample (regionStart + regionLen);
        const float startX      = pixelForSample (regionStart);

        if (e.position.y < topStripHeight)
        {
            const float fadeInX  = pixelForSample (regionStart + layer.fadeInSamples);
            const float fadeOutX = pixelForSample (regionStart + regionLen - layer.fadeOutSamples);
            const float distIn   = std::abs (e.position.x - fadeInX);
            const float distOut  = std::abs (e.position.x - fadeOutX);

            // SPEC_M6_EDIT_WINDOW.md §B: 44px hit areas -- a click in the top
            // strip that isn't actually near either fade handle is not an
            // edit gesture (falls through to DragTarget::none, same as a
            // miss below the strip); it still places the preview point on
            // release via mouseUp's existing "no drag happened" branch.
            dragTarget = (juce::jmin (distIn, distOut) > handleGrabPixels) ? DragTarget::none
                           : (distIn <= distOut ? DragTarget::fadeIn : DragTarget::fadeOut);
        }
        else
        {
            // Owner's start marker: whichever of the two region handles is
            // nearer wins, and only if the click is actually within grab
            // range of it (otherwise it stays a scrub gesture, unchanged).
            const float distStart = std::abs (e.position.x - startX);
            const float distEnd   = std::abs (e.position.x - regionX);
            if (juce::jmin (distStart, distEnd) > handleGrabPixels) dragTarget = DragTarget::none;
            else dragTarget = (distStart < distEnd) ? DragTarget::regionStart : DragTarget::regionEnd;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging) return;

        // click-vs-drag guard (PRODUCT_REQUIREMENTS.md §17 #2, the historical
        // "30.1-second pad bug" defect class): a click without movement past
        // this threshold must never register as an edit, for any handle
        const bool wasMoved = dragMoved;
        if (e.position.getDistanceFrom (dragStartPos) >= movementThresholdPixels)
            dragMoved = true;

        if (! dragMoved) return;

        // roadmap "Undo": fires exactly once, on the FIRST movement of a
        // real edit gesture (not a miss/DragTarget::none, not a plain click)
        // -- BEFORE any pending value is touched below, so the owner can
        // snapshot the untouched layer state.
        if (! wasMoved && dragTarget != DragTarget::none && onEditStarting)
            onEditStarting();

        switch (dragTarget)
        {
            case DragTarget::none:
                // roadmap "scrubbing": a drag that misses every handle used
                // to do nothing at all (a dead gesture) -- repurposed as
                // scrub: follow the mouse continuously (not just on
                // release), and retarget an already-playing preview live
                // via onScrub (real-time-safe hand-off, see
                // scrubPreviewTo()'s own comment).
                previewPosition = sampleForPixel (e.position.x);
                if (onScrub) onScrub (previewPosition);
                repaint();
                break;

            case DragTarget::regionEnd:
            {
                // length measured FROM the start marker, and never allowed to
                // collapse the region to nothing
                const int start = layer.regionStartClamped();
                pendingRegionLength = juce::jlimit (1, juce::jmax (1, layer.numFrames() - start),
                                                     sampleForPixel (e.position.x) - start);
                break;
            }

            case DragTarget::regionStart:
            {
                // must stay at least one sample before the region's end, so
                // the region can never invert or become empty
                const int end = layer.regionStartClamped() + layer.loopLength();
                pendingRegionStart = juce::jlimit (0, juce::jmax (0, end - 1), sampleForPixel (e.position.x));
                break;
            }

            case DragTarget::fadeIn:
            {
                const int maxFade = juce::jmax (1, layer.loopLength() - 1);
                pendingFadeIn = juce::jlimit (0, maxFade, sampleForPixel (e.position.x));
                break;
            }

            case DragTarget::fadeOut:
            {
                const int region  = layer.loopLength();
                const int maxFade = juce::jmax (1, region - 1);
                pendingFadeOut = juce::jlimit (0, maxFade, region - sampleForPixel (e.position.x));
                break;
            }
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragging = false;
        if (dragMoved)
        {
            // single, one-time write per handle on release -- see the
            // thread-safety note above this class: not yet safe if this
            // layer's deck is the one currently playing
            switch (dragTarget)
            {
                case DragTarget::none:
                    break;   // a drag that never found a handle commits nothing
                case DragTarget::regionEnd:
                    // SPEC_WARP_BUG_INVESTIGATION.md fix: setRegionLengthClamped()
                    // (Deck.h) clamps against numFrames() AT THIS EXACT MOMENT --
                    // closes the race with a concurrently-applied re-warp swap
                    // that this class's own header comment already flagged as
                    // "not yet safe if this layer's deck is the one currently
                    // playing." See that method's own comment for the full
                    // root-cause trace.
                    if (pendingRegionLength > 0) { layer.setRegionLengthClamped (pendingRegionLength); layer.trimmed = true; if (onEdited) onEdited(); }
                    break;
                case DragTarget::regionStart:
                {
                    // Same clamp-at-the-moment-of-write discipline as the end
                    // handle. The region's END must not move when the start
                    // does, so regionLength is recomputed to preserve it.
                    if (pendingRegionStart >= 0)
                    {
                        const int oldEnd = layer.regionStartClamped() + layer.loopLength();
                        layer.setRegionStartClamped (pendingRegionStart);
                        layer.setRegionLengthClamped (juce::jmax (1, oldEnd - layer.regionStartClamped()));
                        layer.trimmed = true;
                        if (onEdited) onEdited();
                    }
                    break;
                }
                case DragTarget::fadeIn:
                    if (pendingFadeIn >= 0) { layer.fadeInSamples = pendingFadeIn; if (onEdited) onEdited(); }
                    break;
                case DragTarget::fadeOut:
                    if (pendingFadeOut >= 0) { layer.fadeOutSamples = pendingFadeOut; if (onEdited) onEdited(); }
                    break;
            }
        }
        else
        {
            // a click that never passed the drag threshold places the
            // preview playhead (PRODUCT_REQUIREMENTS.md §7: "Clicking the
            // waveform places the playhead; Preview plays from there") --
            // this is purely local UI state (read by ClipEditorContent's
            // Preview button), not written to the Layer at all
            previewPosition = juce::jlimit (0, juce::jmax (0, layer.numFrames() - 1),
                                             sampleForPixel (dragStartPos.x));
        }
        dragMoved = false;
        pendingRegionStart  = -1;
        pendingRegionLength = -1;
        pendingFadeIn       = -1;
        pendingFadeOut      = -1;
        repaint();
    }

    // sample index the preview playhead was last clicked to, in the full raw
    // buffer's coordinate space (same coordinate space region-end/fade use)
    int getPreviewPosition() const { return previewPosition; }

    // SPEC_M6_EDIT_WINDOW.md §A "changes reflect in the lane's waveform above
    // where practical": fired once per committed region/fade edit (never on
    // preview clicks, which don't touch the Layer) so the owner can refresh
    // whatever else is showing this same Layer's trim state.
    std::function<void()> onEdited;

    // roadmap "Undo": fired once per drag gesture, before any pending value
    // is mutated (see mouseDrag) -- lets the owner snapshot pre-edit state.
    std::function<void()> onEditStarting;

    // roadmap "scrubbing": fired continuously while dragging across the
    // waveform outside any handle (see mouseDrag's DragTarget::none case).
    std::function<void (int)> onScrub;

    // Roadmap "zoom/pan ... waveform": Ctrl/Cmd+wheel zooms (clamped [1x,
    // 32x]), centred approximately on the cursor so zooming feels anchored
    // rather than always re-centring on the buffer's start; plain wheel pans
    // while zoomed in (a no-op at 1x, matching StemLaneView's own
    // "nothing to pan when everything already fits" convention). zoomFactor/
    // viewStartFraction default fresh (1.0/0.0) for every new WaveformView
    // instance -- since the docked editor is destroyed and recreated per
    // lane-swap (openStemDock()), no explicit reset-on-clip-change is needed.
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        if (delta == 0.0f) return;

        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            const double cursorFrac = viewStartFraction + (double) (e.position.x / (float) juce::jmax (1, getWidth())) / (double) zoomFactor;
            zoomFactor = juce::jlimit (1.0f, 32.0f, zoomFactor * (delta > 0.0f ? 1.2f : (1.0f / 1.2f)));
            // re-anchor so the point under the cursor stays under the cursor
            viewStartFraction = cursorFrac - (double) (e.position.x / (float) juce::jmax (1, getWidth())) / (double) zoomFactor;
            clampView();
            repaint();
            if (onViewChanged) onViewChanged();
        }
        else if (zoomFactor > 1.0f)
        {
            viewStartFraction -= (double) delta * 0.1 / (double) zoomFactor;
            clampView();
            repaint();
            if (onViewChanged) onViewChanged();
        }
    }

private:
    enum class DragTarget { none, regionStart, regionEnd, fadeIn, fadeOut };

    void clampView()
    {
        const double visibleFraction = 1.0 / (double) zoomFactor;
        viewStartFraction = juce::jlimit (0.0, juce::jmax (0.0, 1.0 - visibleFraction), viewStartFraction);
    }

    // Roadmap "zoom/pan": sampleIndex -> x now maps through the current
    // [viewStartFraction, viewStartFraction + 1/zoomFactor) window instead
    // of always the whole buffer -- at the default zoomFactor=1/
    // viewStartFraction=0 this is identical to the pre-zoom formula.
    float pixelForSample (int sampleIndex) const
    {
        const int total = layer.numFrames();
        if (total <= 0) return 0.0f;
        const double frac     = (double) sampleIndex / (double) total;
        const double viewFrac = (frac - viewStartFraction) * (double) zoomFactor;
        return (float) getWidth() * (float) viewFrac;
    }

    int sampleForPixel (float x) const
    {
        const int total = layer.numFrames();
        const double viewFrac = (double) x / (double) juce::jmax (1, getWidth());
        const double frac     = juce::jlimit (0.0, 1.0, viewStartFraction + viewFrac / (double) zoomFactor);
        return (int) (frac * (double) total);
    }

    ezdeck::Layer&      layer;
    juce::Colour        accentColour  { 0xff6ee7b7 };   // pre-M6 green default
    double              samplesPerBar { 0.0 };           // 0 = no grid context set yet
    int                 gridDivisionsPerBar { 4 };       // owner's DAW grid: 4 = 1/4 notes (default), 0 = off
    float               zoomFactor        { 1.0f };   // roadmap "zoom/pan" -- 1.0 = whole buffer fit to width
    double              viewStartFraction { 0.0 };    // 0..1, start of the visible window as a fraction of the buffer
    juce::Point<float>  dragStartPos;
    bool                dragging  { false };
    bool                dragMoved { false };
    DragTarget          dragTarget { DragTarget::none };
    int                 pendingRegionLength { -1 };
    int                 pendingFadeIn       { -1 };
    int                 pendingFadeOut      { -1 };
    int                 previewPosition     { 0 };
    int                 pendingRegionStart  { -1 };   // owner's start marker, uncommitted drag value
    int                 livePlayheadSample  { -1 };   // owner's moving playhead; -1 = nothing playing
    bool                livePlayheadActive  { false };

    // matches PRODUCT_REQUIREMENTS.md §17 #2's own cited historical threshold
    static constexpr float movementThresholdPixels = 4.0f;
    static constexpr float topStripHeight           = 28.0f;
    static constexpr float handleGrabPixels         = 22.0f;   // SPEC_M6_EDIT_WINDOW.md §B: 44px hit areas (22px each side of the grip)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

//==============================================================================
//  Clip editor — milestone 3 shell (M3-T4), waveform + region-end/fade
//  handles (M3-T5/M3-T6/M3-T7), now with BPM tools (M3-T8).
//
//  BPM ÷2/×2/Set BPM tag a clip's tempo (taggedBpmRef, owned by
//  SessionComponent, initialized from the same per-layer analyze() result
//  loadLayer already computes for region-fitting). Per PRODUCT_REQUIREMENTS.md
//  §7, tagging the tempo "does not itself stretch the audio" -- there is no
//  warp integration yet (that's IMPLEMENTATION_ROADMAP.md Milestone 5), so
//  this matches the PRD's own description exactly rather than being a gap.
//
//  Fit 4 bars / Snap to grid / Reset markers all operate on the SAME
//  regionLength/trimmed fields the M3-T6 drag handle already writes -- same
//  data race already tracked in project/KNOWN_BUGS.md #13 (extended to note
//  this additional write path), not a new finding requiring a new entry.
//
//  Known scope gap, not fixed here (staying within M3-T8 as literally
//  written in project/MILESTONE_3_EXECUTION_PLAN.md): PRODUCT_REQUIREMENTS.md
//  §7 also lists a "To downbeat" button that isn't assigned to any task in
//  the execution plan (M3-T8 nor any other) -- an omission in the plan
//  itself, flagged rather than silently added or silently left unmentioned.
//
//  Snap to grid, like the M3-T6 region-end handle, can only snap the region
//  *end* -- there is still no region-start concept in Deck.h (see
//  WaveformView's own header comment).
//
//  Preview (M3-T9): clicking the waveform (a click, not a drag -- see
//  WaveformView's mouseUp) sets a preview playhead marker; this button plays
//  the layer's own audio from that marker. Implemented as a separate,
//  additively-mixed one-shot voice in SessionComponent::getNextAudioBlock
//  (see startPreview/stopPreview/renderPreview there) -- it never touches
//  session/Deck/the deck's own playhead, so deck transport is provably
//  unaffected. Plays through to the end of the RAW buffer (not clamped to
//  the current trimmed region), a deliberate choice so Preview can also be
//  used to audition material beyond the current region-end before deciding
//  where to trim it.
//==============================================================================
//==============================================================================
//  Visual-polish pass (LoopLab reference): a pill-shaped, sliding-thumb
//  toggle -- the reference's own .sw component -- standing in for
//  juce::ToggleButton's plain checkbox everywhere a Settings row just needs
//  a real on/off, no label of its own (the row's label is a separate
//  juce::Label alongside it; see SettingsGeneralTab's own layout).
//==============================================================================
class ToggleSwitch : public juce::Component
{
public:
    // Explicit, even though it does nothing extra: JUCE_DECLARE_NON_COPYABLE
    // below deletes the copy constructor, which counts as a user-declared
    // constructor and suppresses the implicit default one too -- same
    // reason DeckCard.h/DeckTriggerCell's own constructors document this.
    ToggleSwitch() = default;

    std::function<void()> onClick;

    void setToggleState (bool on, juce::NotificationType notify)
    {
        if (state == on) return;
        state = on;
        repaint();
        if (notify != juce::dontSendNotification && onClick != nullptr) onClick();
    }
    bool getToggleState() const noexcept { return state; }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = b.getHeight() * 0.5f;

        juce::ColourGradient track (state ? juce::Colour (0xff10b981).withAlpha (0.5f) : juce::Colours::black.withAlpha (0.45f),
                                     b.getX(), b.getY(),
                                     state ? juce::Colour (0xff059669).withAlpha (0.25f) : juce::Colours::white.withAlpha (0.05f),
                                     b.getX(), b.getBottom(), false);
        g.setGradientFill (track);
        g.fillRoundedRectangle (b, radius);
        g.setColour (state ? juce::Colour (0xff10b981) : juce::Colours::white.withAlpha (0.14f));
        g.drawRoundedRectangle (b, radius, 1.0f);

        const float thumbD = b.getHeight() - 4.0f;
        const float thumbX = state ? b.getRight() - thumbD - 2.0f : b.getX() + 2.0f;
        auto thumb = juce::Rectangle<float> (thumbX, b.getY() + 2.0f, thumbD, thumbD);
        juce::ColourGradient tgrad (state ? juce::Colour (0xffa7f3d0) : juce::Colours::white.withAlpha (0.85f),
                                     thumb.getX(), thumb.getY(),
                                     state ? juce::Colour (0xff34d399) : juce::Colours::white.withAlpha (0.4f),
                                     thumb.getX(), thumb.getBottom(), false);
        g.setGradientFill (tgrad);
        g.fillEllipse (thumb);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition())) setToggleState (! state, juce::sendNotification);
    }

private:
    bool state { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToggleSwitch)
};

//==============================================================================
//  Milestone 13: the Settings panel (PRD §14's "Options modal," 4 tabs).
//  General's 4 toggles are backed by real, already-built behavior only
//  (PRD §14's own "every switch takes effect immediately") -- Shortcuts/MIDI
//  are placeholders until Milestone 14 builds the action registry those
//  tabs actually need; a switch with nothing behind it would violate that
//  same acceptance criterion, so none is shown yet rather than a fake one.
//  Audio reuses juce::AudioDeviceSelectorComponent (JUCE's own built-in
//  component, bound to the app's existing juce::AudioDeviceManager) rather
//  than building a device/rate/channel picker from scratch.
//==============================================================================
class SettingsGeneralTab : public juce::Component
{
public:
    // Bug report (PerformLive UI/UX notes): "Meters are one of the most
    // important visual feedback elements during live performance... no need
    // for a toggle to show or hide them" -- the meterVisible show/hide
    // toggle is gone; MixerChannelStrip's own meterShown member still
    // defaults to true and is simply never told otherwise now.
    SettingsGeneralTab (bool metronomeEnabled, bool tempoLockEnabled, bool onePadAtATime)
    {
        auto setUpRow = [this] (juce::Label& label, const juce::String& text)
        {
            label.setText (text, juce::dontSendNotification);
            label.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::plain)));
            label.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ff));
            addAndMakeVisible (label);
        };
        setUpRow (metronomeLabel, "Metronome");
        setUpRow (tempoLockLabel, "Tempo Lock");
        setUpRow (onePadLabel,    "One Pad At A Time");

        metronomeToggle.setToggleState (metronomeEnabled, juce::dontSendNotification);
        metronomeToggle.onClick = [this] { if (onMetronomeChanged) onMetronomeChanged (metronomeToggle.getToggleState()); };
        addAndMakeVisible (metronomeToggle);

        tempoLockToggle.setToggleState (tempoLockEnabled, juce::dontSendNotification);
        tempoLockToggle.onClick = [this] { if (onTempoLockChanged) onTempoLockChanged (tempoLockToggle.getToggleState()); };
        addAndMakeVisible (tempoLockToggle);

        onePadToggle.setToggleState (onePadAtATime, juce::dontSendNotification);
        onePadToggle.onClick = [this] { if (onOnePadChanged) onOnePadChanged (onePadToggle.getToggleState()); };
        addAndMakeVisible (onePadToggle);
    }

    std::function<void (bool)> onMetronomeChanged, onTempoLockChanged, onOnePadChanged;

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto row = [&] (juce::Label& label, ToggleSwitch& sw)
        {
            auto r = area.removeFromTop (28);
            sw.setBounds (r.removeFromRight (42).withSizeKeepingCentre (42, 22));
            label.setBounds (r);
        };
        row (metronomeLabel, metronomeToggle);
        row (tempoLockLabel, tempoLockToggle);
        row (onePadLabel,    onePadToggle);
    }

private:
    juce::Label  metronomeLabel, tempoLockLabel, onePadLabel;
    ToggleSwitch metronomeToggle, tempoLockToggle, onePadToggle;
};

// Milestone 14: PRD §15's own default action names, for the Shortcuts/MIDI
// tabs' display and for the "which ActionId does this row mean" learn flow.
// Deck-slot/Pad/FX/Scene names are generated, not hand-typed, since there
// are 40 of them following one obvious naming pattern.
inline juce::String actionDisplayName (ezaction::ActionId id)
{
    using namespace ezaction;
    switch (id)
    {
        case ActionId::PlayStop:         return "Play/Stop";
        case ActionId::PlayNext:         return "Play Next (Advance)";
        case ActionId::NextDeck:         return "Next Deck";
        case ActionId::PrevDeck:         return "Previous Deck";
        case ActionId::TapTempo:         return "Tap Tempo";
        case ActionId::ToggleMetronome:  return "Metronome";
        case ActionId::ToggleTempoLock:  return "Tempo Lock";
        case ActionId::AllPadsOff:       return "All Pads Off";
        case ActionId::NextSection:      return "Next Section";
        case ActionId::PrevSection:      return "Previous Section";
        case ActionId::NextSong:         return "Next Song";
        case ActionId::PrevSong:         return "Previous Song";
        case ActionId::JumpNow:          return "Jump Now";
        case ActionId::CancelJump:       return "Cancel Jump";
        case ActionId::LoopSection:      return "Loop Section";
        case ActionId::CountInPlay:      return "Play with Count-In";
        case ActionId::TogglePlaybackView: return "Playback View";
        default: break;
    }
    const int i = (int) id;
    if (i >= (int) ActionId::DeckSlotBase && i < (int) ActionId::PadBase)
    {
        const int slot = i - (int) ActionId::DeckSlotBase;
        return "Deck " + juce::String (slot < 4 ? "A" : "B") + juce::String (slot % 4 + 1);
    }
    if (i >= (int) ActionId::PadBase && i < (int) ActionId::FxBase)
        return "Pad " + juce::String (i - (int) ActionId::PadBase + 1);
    if (i >= (int) ActionId::FxBase && i < (int) ActionId::SceneBase)
        return "FX " + juce::String (i - (int) ActionId::FxBase + 1);
    if (i >= (int) ActionId::SceneBase && i < (int) ActionId::kCount)
        return "Scene " + juce::String (i - (int) ActionId::SceneBase + 1);
    return "(unknown)";
}

inline std::vector<ezaction::ActionId> allActionIds()
{
    std::vector<ezaction::ActionId> ids;
    for (int i = 0; i < ezaction::kNumActions; ++i) ids.push_back ((ezaction::ActionId) i);
    return ids;
}

//==============================================================================
//  Milestone 14: Shortcuts tab -- one row per action (PRD §15's own default
//  keymap table, 48 actions), each showing its current key binding with
//  Rebind/Clear buttons, plus "Restore Defaults" (KeyBindingMap's own
//  resetToDefaults(), reproducing PRD §15's table exactly). Rebinding uses a
//  text-entry prompt (juce::KeyPress::createFromDescription(), e.g. "space",
//  "up", "q") rather than live "press any key" capture -- a deliberate,
//  lower-risk choice given this environment has no way to interactively
//  verify keyboard-focus routing inside a nested modal component.
//==============================================================================
class ShortcutsTab : public juce::Component
{
public:
    ShortcutsTab (ezaction::KeyBindingMap& bindingsToUse, std::function<void()> onChangedCallback)
        : keyBindings (bindingsToUse), onChanged (std::move (onChangedCallback))
    {
        restoreButton.setButtonText ("Restore Defaults");
        restoreButton.onClick = [this] { keyBindings.resetToDefaults(); rebuildRows(); if (onChanged) onChanged(); };
        addAndMakeVisible (restoreButton);

        viewport.setViewedComponent (&rowsHolder, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        rebuildRows();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        restoreButton.setBounds (area.removeFromTop (28));
        area.removeFromTop (4);
        viewport.setBounds (area);
        rowsHolder.setSize (viewport.getWidth() - 20, (int) allActionIds().size() * 24);
    }

private:
    void rebuildRows()
    {
        rowsHolder.removeAllChildren();
        nameLabels.clear();
        keyButtons.clear();
        clearButtons.clear();

        int y = 0;
        for (auto id : allActionIds())
        {
            auto* nameLabel = new juce::Label ({}, actionDisplayName (id));
            nameLabel->setBounds (0, y, 160, 22);
            rowsHolder.addAndMakeVisible (nameLabel);
            nameLabels.add (nameLabel);

            juce::String keyText = "(unbound)";
            for (auto& kv : keyBindings.all())
                if (kv.second == id) { keyText = kv.first; break; }

            auto* keyButton = new juce::TextButton (keyText);
            keyButton->setBounds (168, y, 110, 22);
            keyButton->onClick = [this, id, keyButton] { promptRebind (id, keyButton); };
            rowsHolder.addAndMakeVisible (keyButton);
            keyButtons.add (keyButton);

            auto* clearButton = new juce::TextButton ("Clear");
            clearButton->setBounds (282, y, 60, 22);
            clearButton->onClick = [this, id, keyButton]
            {
                // remove any binding currently pointing at this action
                auto copy = keyBindings.all();
                for (auto& kv : copy) if (kv.second == id) keyBindings.unbind (juce::KeyPress::createFromDescription (kv.first));
                keyButton->setButtonText ("(unbound)");
                if (onChanged) onChanged();
            };
            rowsHolder.addAndMakeVisible (clearButton);
            clearButtons.add (clearButton);

            y += 24;
        }
    }

    void promptRebind (ezaction::ActionId id, juce::TextButton* keyButton)
    {
        auto* aw = new juce::AlertWindow ("Rebind " + actionDisplayName (id),
                                           "Type the key (e.g. \"space\", \"up\", \"q\"):",
                                           juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("key", keyButton->getButtonText());
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, id, keyButton] (int result)
            {
                if (result == 1)
                {
                    const juce::String text = aw->getTextEditorContents ("key").trim();
                    const auto key = juce::KeyPress::createFromDescription (text);
                    if (key != juce::KeyPress())
                    {
                        keyBindings.bind (key, id);
                        keyButton->setButtonText (ezaction::KeyBindingMap::keyToString (key));
                        if (onChanged) onChanged();
                    }
                }
                delete aw;
            }), false);
    }

    ezaction::KeyBindingMap& keyBindings;
    std::function<void()> onChanged;
    juce::TextButton restoreButton;
    juce::Viewport viewport;
    juce::Component rowsHolder;
    juce::OwnedArray<juce::Label> nameLabels;
    juce::OwnedArray<juce::TextButton> keyButtons, clearButtons;
};

//==============================================================================
//  Milestone 14: MIDI tab -- connection status (which devices are open) plus
//  a representative learn flow (learn a button-style action, or learn
//  Master Gain as a continuous fader) proving the mechanism PRD §15
//  describes. Per-channel fader learn for every mixer strip individually is
//  a natural follow-on refinement, not built here.
//==============================================================================
class MidiTab : public juce::Component
{
public:
    MidiTab (ezaction::MidiActionRouter& routerToUse, const juce::StringArray& openDeviceNames,
              std::function<void (float)> masterGainSetter)
        : router (routerToUse)
    {
        statusLabel.setText (openDeviceNames.isEmpty() ? "No MIDI input devices open"
                                                        : "Open: " + openDeviceNames.joinIntoString (", "),
                             juce::dontSendNotification);
        addAndMakeVisible (statusLabel);

        actionBox.addItemList (buildActionNames(), 1);
        actionBox.setSelectedId (1);
        addAndMakeVisible (actionBox);

        learnButtonButton.setButtonText ("Learn Button (next CC>63 or note)");
        learnButtonButton.onClick = [this]
        {
            const auto id = allActionIds()[(size_t) juce::jmax (0, actionBox.getSelectedId() - 1)];
            router.armLearnButton (id);
            learnButtonButton.setButtonText ("Listening...");
        };
        addAndMakeVisible (learnButtonButton);

        learnFaderButton.setButtonText ("Learn Fader for Master Gain");
        learnFaderButton.onClick = [this, setter = std::move (masterGainSetter)] () mutable
        {
            router.armLearnFader (setter);
            learnFaderButton.setButtonText ("Listening...");
        };
        addAndMakeVisible (learnFaderButton);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        statusLabel.setBounds (area.removeFromTop (24));
        actionBox.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);
        learnButtonButton.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);
        learnFaderButton.setBounds (area.removeFromTop (28));
    }

private:
    static juce::StringArray buildActionNames()
    {
        juce::StringArray names;
        for (auto id : allActionIds()) names.add (actionDisplayName (id));
        return names;
    }

    ezaction::MidiActionRouter& router;
    juce::Label statusLabel;
    juce::ComboBox actionBox;
    juce::TextButton learnButtonButton, learnFaderButton;
};

class SettingsPanelContent : public juce::Component
{
public:
    SettingsPanelContent (bool metronomeEnabled, bool tempoLockEnabled, bool onePadAtATime,
                           juce::AudioDeviceManager& deviceManager,
                           ezaction::KeyBindingMap& keyBindings, std::function<void()> onKeyBindingsChanged,
                           ezaction::MidiActionRouter& midiRouter, const juce::StringArray& openMidiDeviceNames,
                           std::function<void (float)> masterGainSetter)
        : tabs (juce::TabbedButtonBar::TabsAtTop)
    {
        auto* general = new SettingsGeneralTab (metronomeEnabled, tempoLockEnabled, onePadAtATime);
        generalTab = general;
        tabs.addTab ("General", juce::Colour (0xff202030), general, true);

        // Owner: "the app doesn't see my UMC driver... it should see different
        // sound card drivers like other DAWs and the outputs the device has."
        // Two things were wrong here. maxOutputChannels was 2, which told JUCE
        // to offer at most a stereo pair no matter what the interface has, and
        // hideAdvancedOptions was true, which hid the driver-type dropdown
        // (ASIO / Windows Audio / DirectSound) and the per-channel enable
        // list. Now: up to 64 outputs, shown as stereo pairs (that is how the
        // mixer routes them), driver type visible, and the ASIO control panel
        // button when the driver has one.
        auto* audio = new juce::AudioDeviceSelectorComponent (deviceManager,
                                                              0, 0,        // no inputs
                                                              2, 64,       // outputs: at least stereo, up to 64
                                                              false,       // no MIDI inputs here (MIDI has its own tab)
                                                              false,
                                                              true,        // stereo pairs -- matches Out 1/2, 3/4...
                                                              false);      // show driver type + advanced options
        tabs.addTab ("Audio", juce::Colour (0xff202030), audio, true);

        tabs.addTab ("Shortcuts", juce::Colour (0xff202030), new ShortcutsTab (keyBindings, std::move (onKeyBindingsChanged)), true);
        tabs.addTab ("MIDI", juce::Colour (0xff202030), new MidiTab (midiRouter, openMidiDeviceNames, std::move (masterGainSetter)), true);

        // Owner: "same as the settings window [feels foreign]" -- the tab
        // strip's empty area right of the last tab painted in the stock
        // light grey; retint the bar itself to the app's shell colours.
        tabs.setColour (juce::TabbedComponent::backgroundColourId, juce::Colour (0xff0c0c17));   // == performlive::kShellBg
        tabs.setColour (juce::TabbedComponent::outlineColourId,    juce::Colour (0xff2b2b4d));   // == performlive::kBorder
        tabs.getTabbedButtonBar().setColour (juce::TabbedButtonBar::tabTextColourId,      juce::Colour (0xffa3a6cc));   // == kTextDim
        tabs.getTabbedButtonBar().setColour (juce::TabbedButtonBar::frontTextColourId,    juce::Colour (0xfff2f0ff));   // == kTextBright

        addAndMakeVisible (tabs);
        setSize (480, 420);
    }

    SettingsGeneralTab& general() { return *generalTab; }

    void resized() override { tabs.setBounds (getLocalBounds()); }

private:
    juce::TabbedComponent tabs;
    SettingsGeneralTab* generalTab { nullptr };
};

//==============================================================================
//  UI_SPEC_LIBRARY.md §3: one sample card in the LIBRARY grid. Peak-cache
//  approach copied from DeckCard.h (kPeakResolution buckets, computed once
//  in setAudio(), paint() only ever reads the cache) -- a PARALLEL cache,
//  not a factored-out shared helper: DeckCard's own state machine (empty/
//  armed/live, tied to deck arm/trigger semantics) doesn't fit a library
//  browser's single "auditioning or not" state, and forcing one to wrap the
//  other seemed more invasive than copying the small, proven pattern (the
//  spec's own §3 explicitly allows either choice -- this is the one taken).
//  Long-press (650ms) mirrors DeckCard's own hold pattern, same touch-parity
//  reason DeckCard's header documents.
//==============================================================================
class LibrarySampleCard : public juce::Component, private juce::Timer
{
public:
    // UI_SPEC_LIBRARY.md §4: list view is "denser, one row per sample" --
    // same card, same peak-cache, same audition/menu wiring, just a
    // different paint()/geometry. One class for both (not a second parallel
    // class) since everything except layout is identical.
    enum class LayoutMode { grid, list };

    LibrarySampleCard() { setWantsKeyboardFocus (false); }

    std::function<void()> onTap;    // audition toggle
    std::function<void()> onMenu;   // long-press, right-click -- assign/edit/delete/re-tag
    std::function<void()> onToggleSelect;   // ctrl/shift-click: add to or take out of the browser's selection

    void setEntry (const ezlibrary::LibraryEntry& e)
    {
        assetId  = e.assetId;
        name     = e.name;
        bpm      = e.detectedBpm;
        category = e.category;
        key      = e.key;
        favorite = e.favorite;
        hasCollection = e.collectionId.isNotEmpty();
        collectionColour = hasCollection ? collectionColorFor (e.collectionId) : juce::Colours::transparentBlack;
        repaint();
    }

    // Phase 1.1 P2 "a shared color" for an auto-detected stem-set collection
    // -- deterministic from the collection's own id (a hash-derived hue), so
    // every card in the same collection renders the identical colour without
    // storing it redundantly on every LibraryEntry.
    static juce::Colour collectionColorFor (const juce::String& collectionId)
    {
        const int32_t h = (int32_t) collectionId.hashCode();
        return juce::Colour::fromHSV ((float) ((uint32_t) h % 360u) / 360.0f, 0.55f, 0.90f, 1.0f);
    }

    const juce::String& getAssetId() const { return assetId; }

    void setAuditioning (bool a) { if (auditioning != a) { auditioning = a; repaint(); } }
    void setLayoutMode (LayoutMode m) { if (mode != m) { mode = m; repaint(); } }
    void setDurationSeconds (double d) { durationSeconds = d; repaint(); }
    // §4: "alternating fill kCard / kShellBg" -- list rows only; ignored in grid mode.
    void setAlternateShade (bool alt) { if (altShade != alt) { altShade = alt; repaint(); } }
    void setSelected (bool s) { if (selected != s) { selected = s; repaint(); } }

    // Message-thread only (called once per refresh, never per-frame) --
    // builds the peak cache once, exactly like DeckCard::setAudio().
    void setAudio (const std::vector<float>& samples)
    {
        peaksMin.assign (kPeakResolution, 0.0f);
        peaksMax.assign (kPeakResolution, 0.0f);
        hasAudio = ! samples.empty();
        if (! hasAudio) { repaint(); return; }

        const size_t n = samples.size();
        for (int b = 0; b < kPeakResolution; ++b)
        {
            const size_t start = (size_t) ((double) b       / kPeakResolution * (double) n);
            const size_t end   = (size_t) ((double) (b + 1) / kPeakResolution * (double) n);
            float mn = 0.0f, mx = 0.0f;
            for (size_t i = start; i < end && i < n; ++i)
            {
                const float v = samples[i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            peaksMin[(size_t) b] = mn;
            peaksMax[(size_t) b] = mx;
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (mode == LayoutMode::list) { paintListRow (g); return; }

        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = 8.0f;

        // Visual-polish pass (LoopLab reference): a real ambient drop
        // shadow plus a top-lit gradient fill, same "raised glass" language
        // as the rest of this pass -- was a flat kCard/kCardHover rectangle.
        {
            juce::Path shadowPath;
            shadowPath.addRoundedRectangle (bounds, radius);
            juce::DropShadow (juce::Colours::black.withAlpha (0.3f), 5, { 0, 2 }).drawForPath (g, shadowPath);
        }
        const juce::Colour base (hovering ? 0xff23263au : 0xff151527u);   // kCardHover / kCard
        juce::ColourGradient cardGrad (base.brighter (0.08f), bounds.getX(), bounds.getY(),
                                        base.darker (0.06f), bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill (cardGrad);
        g.fillRoundedRectangle (bounds, radius);

        g.setColour (juce::Colour (auditioning ? 0xff2ee86au : 0xff2b2b4du));   // kPlay / kBorder
        g.drawRoundedRectangle (bounds, radius, auditioning ? 2.0f : 1.0f);

        // Phase 1.1 P2 "a shared color" for an auto-detected stem-set
        // collection -- same left-edge accent-stripe idiom DeckTriggerCell's
        // own "Color Row" feature already uses (Phase 1.1 P1), for a
        // consistent visual language across both features.
        if (hasCollection)
        {
            auto stripe = getLocalBounds().toFloat().removeFromLeft (4.0f).reduced (1.0f, 4.0f);
            g.setColour (collectionColour);
            g.fillRoundedRectangle (stripe, 2.0f);
        }

        auto area = getLocalBounds().reduced (8, 6);
        auto waveArea = area.removeFromTop (72);
        drawWaveform (g, waveArea.toFloat());

        // Visual-polish pass (LoopLab reference): BPM as its own bold
        // monospace badge over the waveform's top-right corner, not buried
        // in the small grey meta line below -- the reference's own cards
        // lead with tempo, it's the single fact a performer scans for
        // fastest when picking a loop.
        if (bpm > 0.0)
        {
            const juce::String bpmText = juce::String (bpm, 0) + " BPM";
            juce::Font bpmFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::bold));
            g.setFont (bpmFont);
            const int textW = juce::GlyphArrangement::getStringWidthInt (bpmFont, bpmText);
            auto badge = juce::Rectangle<int> (0, 0, textW + 10, 15)
                             .withRightX (waveArea.getRight() - 3).withY (waveArea.getY() + 3);
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRoundedRectangle (badge.toFloat(), 4.0f);
            g.setColour (juce::Colour (0xffc7d2feu));   // light indigo -- reads as "data" against the dark badge
            g.drawText (bpmText, badge, juce::Justification::centred, false);
        }

        area.removeFromTop (4);

        if (favorite)
        {
            auto starArea = area.removeFromTop (14).removeFromRight (16);
            g.setColour (juce::Colour (0xffffc933u));   // kQueued, amber star
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x98\x85")), starArea, juce::Justification::centred, false);
        }

        g.setColour (juce::Colour (0xfff2f0ffu));   // kTextBright
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (name, area.removeFromTop (16), juce::Justification::centredLeft, true);

        g.setColour (juce::Colour (0xff6f7099u));   // kTextFaint
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::plain)));
        g.drawText (metaLine(), area, juce::Justification::centredLeft, false);
        paintSelection (g);
    }

    // Phase 1.1 P2 "List view reorder: Name, Waveform, Tempo, Key, Category,
    // Duration (waveform currently appears before name -- wrong order)."
    // Was mini-waveform FIRST, then name, then BPM/category/duration/play --
    // waveform now sits between name and tempo, matching the brief's column
    // order left-to-right exactly. Key is a real column now too (Phase
    // 1.1 P2-b added LibraryEntry::key -- the OLD comment here explaining
    // why key had no column, "the same structural-absence reason §3's card
    // omits it," no longer applies once the field actually exists). Play
    // stays the rightmost control -- it's the audition action, not a data
    // column the brief's list is enumerating.
    void paintListRow (juce::Graphics& g)
    {
        auto bounds = getLocalBounds();
        g.setColour (juce::Colour (altShade ? 0xff151527u : 0xff0c0c17u));   // kCard / kShellBg
        g.fillRect (bounds);
        if (hovering) { g.setColour (juce::Colour (0xff23263au).withAlpha (0.6f)); g.fillRect (bounds); }   // kCardHover, mouse nicety only

        if (hasCollection)
        {
            auto stripe = bounds.toFloat().removeFromLeft (3.0f).reduced (0.0f, 2.0f);
            g.setColour (collectionColour);
            g.fillRect (stripe);
        }

        auto area = bounds.reduced (8, 2);

        auto playArea = area.removeFromRight (44);
        g.setColour (juce::Colour (auditioning ? 0xff2ee86au : 0xffa3a6ccu));   // kPlay / kTextDim
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (juce::String (juce::CharPointer_UTF8 (auditioning ? "\xe2\x96\xa0" : "\xe2\x96\xb6")),
                    playArea, juce::Justification::centred, false);

        auto durationArea = area.removeFromRight (44);
        g.setColour (juce::Colour (0xff6f7099u));   // kTextFaint
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (durationSeconds > 0.0 ? formatDuration (durationSeconds) : juce::String(), durationArea, juce::Justification::centred, false);

        // Phase 1.1 P2 typography-clipping pass: elide (true) rather than
        // hard-clip (false) -- a long category string in this fixed 70px
        // slice previously had no ellipsis fallback at all.
        auto categoryArea = area.removeFromRight (70);
        g.drawText (category, categoryArea, juce::Justification::centred, true);

        auto keyArea = area.removeFromRight (44);
        g.drawText (key, keyArea, juce::Justification::centred, true);

        auto bpmArea = area.removeFromRight (56);
        g.drawText (bpm > 0.0 ? juce::String (bpm, 0) + " BPM" : juce::String(), bpmArea, juce::Justification::centred, false);

        area.removeFromRight (8);
        auto waveformArea = area.removeFromRight (80);
        drawWaveform (g, waveformArea.toFloat());

        area.removeFromRight (10);
        g.setColour (juce::Colour (0xfff2f0ffu));   // kTextBright
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        juce::String label = name;
        if (favorite) label = juce::String (juce::CharPointer_UTF8 ("\xe2\x98\x85 ")) + label;
        g.drawText (label, area, juce::Justification::centredLeft, true);
        paintSelection (g);
    }

    void resized() override {}

    void mouseEnter (const juce::MouseEvent&) override { hovering = true; repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovering = false; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onMenu != nullptr) onMenu(); return; }
        held = false;
        dragStarted = false;
        startTimer (kHoldMs);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        stopTimer();
        if (held || dragStarted || e.mods.isPopupMenu()) return;
        if (! getLocalBounds().contains (e.getPosition())) return;
        if ((e.mods.isCommandDown() || e.mods.isShiftDown()) && onToggleSelect != nullptr) { onToggleSelect(); return; }
        if (onTap != nullptr) onTap();
    }

    // Phase 1.1 P1 "Deck Loading": drag this sample onto a deck. Same
    // distance-threshold pattern JUCE's own drag-and-drop demos use, so a
    // plain click/tap (no movement) still reaches mouseUp's onTap path
    // untouched -- only a real drag cancels the long-press timer and
    // suppresses onTap. The description carries just this card's assetId,
    // prefixed so DeckCard::isInterestedInDragSource() can recognize this
    // specific drag-source vocabulary and ignore anything else.
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || dragStarted) return;
        if (e.getDistanceFromDragStart() < 8) return;

        stopTimer();
        held = true;
        dragStarted = true;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            container->startDragging ("ezplay-asset:" + assetId, this);
    }

private:
    void timerCallback() override { stopTimer(); held = true; if (onMenu != nullptr) onMenu(); }

    // Selected for a folder move: an indigo wash and outline, plus a tick
    // badge on grid cards (a list row has no spare corner for one).
    void paintSelection (juce::Graphics& g) const
    {
        if (! selected) return;
        const bool grid = mode == LayoutMode::grid;
        const auto b = getLocalBounds().toFloat().reduced (1.0f);
        const float radius = grid ? 8.0f : 0.0f;
        g.setColour (juce::Colour (0xff7c5cffu).withAlpha (0.18f));   // kIndigo
        g.fillRoundedRectangle (b, radius);
        g.setColour (juce::Colour (0xff7c5cffu));
        g.drawRoundedRectangle (b, radius, 2.0f);
        if (! grid) return;

        const juce::Rectangle<float> badge (b.getX() + 7.0f, b.getY() + 7.0f, 18.0f, 18.0f);
        g.fillEllipse (badge);
        juce::Path tick;
        tick.startNewSubPath (badge.getX() + 5.0f, badge.getCentreY() + 0.5f);
        tick.lineTo (badge.getX() + 8.0f, badge.getBottom() - 5.0f);
        tick.lineTo (badge.getRight() - 4.5f, badge.getY() + 5.5f);
        g.setColour (juce::Colours::white);
        g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // §3: "Show only fields that exist; omit absent segments -- no '-'
    // spam." Phase 1.1 P2-b gave LibraryEntry a real (user-entered, still
    // never auto-detected) key field, but the grid card's own compact
    // meta-line is out of this pass's scope (the brief's own "list view
    // reorder" is specifically about the LIST row, added as a real column
    // there instead) -- left as bpm/category only here, unchanged.
    juce::String metaLine() const
    {
        juce::StringArray parts;
        if (bpm > 0.0) parts.add (juce::String (bpm, 0) + " BPM");
        if (category.isNotEmpty()) parts.add (category);
        return parts.joinIntoString (juce::CharPointer_UTF8 (" \xc2\xb7 "));
    }

    static juce::String formatDuration (double seconds)
    {
        const int total = (int) std::llround (seconds);
        return juce::String (total / 60) + ":" + (total % 60 < 10 ? "0" : "") + juce::String (total % 60);
    }

    void drawWaveform (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        if (! hasAudio || peaksMax.empty() || r.getHeight() < 4.0f) return;
        g.setColour (juce::Colour (0xff7c5cffu));   // kIndigo, per §3's mockup
        const float midY  = r.getCentreY();
        const float halfH = r.getHeight() * 0.5f;
        const int   w     = juce::jmax (1, (int) r.getWidth());
        for (int x = 0; x < w; ++x)
        {
            const int b = juce::jlimit (0, kPeakResolution - 1, (int) ((double) x / (double) w * kPeakResolution));
            const float mx = peaksMax[(size_t) b];
            const float mn = peaksMin[(size_t) b];
            const float y1 = midY - mx * halfH * 0.92f;
            const float y2 = midY - mn * halfH * 0.92f;
            g.drawLine (r.getX() + (float) x, y1, r.getX() + (float) x, juce::jmax (y2, y1 + 1.0f), 1.0f);
        }
    }

    static constexpr int kPeakResolution = 320;   // matches DeckCard's own resolution
    static constexpr int kHoldMs         = 650;   // matches DeckCard/SceneButton's own hold pattern

    juce::String assetId, name, category, key;
    double bpm { 0.0 };
    double durationSeconds { 0.0 };
    bool favorite    { false };
    bool auditioning { false };
    bool hovering    { false };
    bool held        { false };
    bool altShade    { false };
    bool selected    { false };
    bool dragStarted { false };
    bool hasCollection { false };
    juce::Colour collectionColour { juce::Colours::transparentBlack };
    LayoutMode mode  { LayoutMode::grid };
    std::vector<float> peaksMin, peaksMax;
    bool hasAudio { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibrarySampleCard)
};

//==============================================================================
//  A folder in the LIBRARY browser: a named group of samples (usually one
//  song's stems) with an optional picture. Tap opens it; right-click or a
//  press-and-hold opens its menu; dropping a sample card on it moves that
//  sample in. Same card size and list-row height as LibrarySampleCard so the
//  two share one grid.
//==============================================================================
class LibraryFolderCard : public juce::Component,
                          public juce::DragAndDropTarget,
                          private juce::Timer
{
public:
    LibraryFolderCard() = default;

    std::function<void()> onOpen;
    std::function<void()> onMenu;
    std::function<void (const juce::String&)> onAssetDropped;

    // anyMemberAssetId: dragging the folder onto a deck drags this sample, and
    // a deck given any folder member loads the whole folder into its row
    // (SessionComponent::loadAssetIntoDeckSlot -> loadPackIntoRow).
    void setFolder (const juce::String& folderName, int itemCount, juce::Colour accentColour, const juce::Image& pictureIn,
                    const juce::String& anyMemberAssetId)
    {
        dragAssetId = anyMemberAssetId;
        name = folderName;
        count = itemCount;
        accent = accentColour;
        picture = pictureIn;
        repaint();
    }

    void setListMode (bool listRow) { if (listMode != listRow) { listMode = listRow; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const juce::String countText = juce::String (count) + (count == 1 ? " item" : " items");

        if (listMode)
        {
            g.setColour (juce::Colour (hovering ? 0xff23263au : 0xff151527u));   // kCardHover / kCard
            g.fillRect (getLocalBounds());
            auto area = getLocalBounds().reduced (8, 4);
            drawCover (g, area.removeFromLeft (36).toFloat(), 5.0f);
            area.removeFromLeft (10);
            drawChevron (g, area.removeFromRight (24).toFloat());
            g.setColour (juce::Colour (0xff6f7099u));   // kTextFaint
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (countText, area.removeFromRight (70), juce::Justification::centredRight, false);
            g.setColour (juce::Colour (0xfff2f0ffu));   // kTextBright
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (name, area, juce::Justification::centredLeft, true);
            if (dropHover) { g.setColour (juce::Colour (0xff7c5cffu)); g.drawRect (getLocalBounds(), 2); }
            return;
        }

        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        {
            juce::Path shadowPath;
            shadowPath.addRoundedRectangle (bounds, 8.0f);
            juce::DropShadow (juce::Colours::black.withAlpha (0.3f), 5, { 0, 2 }).drawForPath (g, shadowPath);
        }
        g.setColour (juce::Colour (hovering ? 0xff23263au : 0xff151527u));
        g.fillRoundedRectangle (bounds, 8.0f);

        auto area = getLocalBounds().reduced (6);
        drawCover (g, area.removeFromTop (area.getHeight() - 38).toFloat(), 6.0f);
        area.removeFromTop (4);
        g.setColour (juce::Colour (0xfff2f0ffu));
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (name, area.removeFromTop (16).withTrimmedLeft (2), juce::Justification::centredLeft, true);
        g.setColour (juce::Colour (0xff6f7099u));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (juce::String ("FOLDER") + juce::String (juce::CharPointer_UTF8 (" " "\xc2" "\xb7" " ")) + countText,
                    area.withTrimmedLeft (2), juce::Justification::centredLeft, false);

        g.setColour (dropHover ? juce::Colour (0xff7c5cffu) : accent.withAlpha (0.5f));
        g.drawRoundedRectangle (bounds, 8.0f, dropHover ? 2.5f : 1.2f);
    }

    void mouseEnter (const juce::MouseEvent&) override { hovering = true; repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovering = false; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onMenu != nullptr) onMenu(); return; }
        held = false;
        dragStarted = false;
        startTimer (kHoldMs);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || dragStarted || e.getDistanceFromDragStart() < 8) return;
        stopTimer();
        dragStarted = true;
        if (dragAssetId.isEmpty()) return;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            container->startDragging ("ezplay-asset:" + dragAssetId, this);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        stopTimer();
        if (held || dragStarted || e.mods.isPopupMenu()) return;
        if (getLocalBounds().contains (e.getPosition()) && onOpen != nullptr) onOpen();
    }

    // Sample cards drag with the "ezplay-asset:" description DeckCard reads.
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return details.description.toString().startsWith ("ezplay-asset:");
    }
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override  { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails& details) override
    {
        dropHover = false;
        repaint();
        if (onAssetDropped != nullptr)
            onAssetDropped (details.description.toString().fromFirstOccurrenceOf ("ezplay-asset:", false, false));
    }

private:
    void timerCallback() override { stopTimer(); held = true; if (onMenu != nullptr) onMenu(); }

    // The folder's picture, cropped to fill; without one, the folder's own
    // colour with a folder shape.
    void drawCover (juce::Graphics& g, juce::Rectangle<float> r, float radius) const
    {
        if (r.getWidth() < 4.0f || r.getHeight() < 4.0f) return;
        juce::Graphics::ScopedSaveState saved (g);
        juce::Path clip;
        clip.addRoundedRectangle (r, radius);
        g.reduceClipRegion (clip);

        if (picture.isValid())
        {
            g.drawImage (picture, r, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
            return;
        }

        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.45f), r.getX(), r.getY(),
                                                 juce::Colour (0xff0c0c17u), r.getRight(), r.getBottom(), false));
        g.fillRect (r);

        const float h = juce::jmin (r.getHeight() * 0.42f, r.getWidth() * 0.34f);
        const auto icon = juce::Rectangle<float> (h * 1.3f, h).withCentre (r.getCentre());
        const float tabH = icon.getHeight() * 0.2f;
        juce::Path shape;
        shape.addRoundedRectangle (icon.getX(), icon.getY(), icon.getWidth() * 0.42f, tabH * 2.0f, tabH * 0.5f);
        shape.addRoundedRectangle (icon.getX(), icon.getY() + tabH, icon.getWidth(), icon.getHeight() - tabH, tabH * 0.5f);
        g.setColour (accent.withAlpha (0.9f));
        g.fillPath (shape);
    }

    static void drawChevron (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto c = r.getCentre();
        juce::Path p;
        p.startNewSubPath (c.x - 3.0f, c.y - 6.0f);
        p.lineTo (c.x + 3.0f, c.y);
        p.lineTo (c.x - 3.0f, c.y + 6.0f);
        g.setColour (juce::Colour (0xffa3a6ccu));   // kTextDim
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    static constexpr int kHoldMs = 650;   // matches LibrarySampleCard

    juce::String name, dragAssetId;
    int count { 0 };
    juce::Colour accent { 0xff7c5cffu };
    juce::Image picture;
    bool listMode    { false };
    bool hovering    { false };
    bool held        { false };
    bool dragStarted { false };
    bool dropHover   { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryFolderCard)
};

//==============================================================================
//  Phase 1.1 P2 "Search bar redesign: match design language, rounded modern
//  styling, instant filtering, spacing, search icon, clear button." The
//  Library's own juce::TextEditor had none of that -- default square
//  corners, no icon, no way to clear except selecting all and deleting.
//  A borderless/transparent TextEditor sits inside this component's own
//  rounded-pill paint(), with a hand-drawn magnifying-glass icon (matching
//  this codebase's own established "hand-drawn icon, not a Unicode glyph"
//  convention -- the header's speaker icon and gear glyph both hit exactly
//  this fallback problem earlier this sprint) and a real Close button that
//  only appears once there's text to clear. "Instant filtering" was already
//  true (TextEditor::onTextChange already drove refreshCards() on every
//  keystroke) -- forwarded through unchanged via this component's own
//  onTextChange, not re-implemented.
//==============================================================================
class SearchBar : public juce::Component
{
public:
    std::function<void()> onTextChange;

    SearchBar()
    {
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::textColourId, juce::Colour (0xfff2f0ffu));   // kTextBright
        editor.onTextChange = [this]
        {
            clearButton.setVisible (editor.getText().isNotEmpty());
            if (onTextChange != nullptr) onTextChange();
        };
        addAndMakeVisible (editor);

        clearButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));   // U+00D7, "x"
        clearButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        clearButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff6f7099u));   // kTextFaint
        clearButton.onClick = [this]
        {
            editor.clear();
            clearButton.setVisible (false);
            if (onTextChange != nullptr) onTextChange();
        };
        clearButton.setVisible (false);
        addAndMakeVisible (clearButton);
    }

    void setPlaceholder (const juce::String& text) { editor.setTextToShowWhenEmpty (text, juce::Colour (0xff6f7099u)); }
    juce::String getText() const { return editor.getText(); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = bounds.getHeight() * 0.5f;   // fully-rounded "pill" -- the brief's own "rounded modern styling"

        g.setColour (juce::Colour (0xff151527u));   // kCard
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (juce::Colour (editor.hasKeyboardFocus (false) ? 0xff7c5cffu : 0xff2b2b4du));   // kIndigo when focused / kBorder otherwise
        g.drawRoundedRectangle (bounds, radius, 1.0f);

        // hand-drawn magnifying glass -- circle + handle stroke
        g.setColour (juce::Colour (0xff6f7099u));   // kTextFaint
        const float cx = bounds.getX() + 17.0f, cy = bounds.getCentreY() - 1.5f;
        g.drawEllipse (cx - 5.0f, cy - 5.0f, 8.5f, 8.5f, 1.4f);
        g.drawLine (cx + 2.3f, cy + 2.3f, cx + 6.5f, cy + 6.5f, 1.6f);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        if (clearButton.isVisible()) clearButton.setBounds (area.removeFromRight (30).reduced (4));
        area.removeFromLeft (32);   // room for the magnifying-glass icon painted above
        editor.setBounds (area.reduced (0, 4));
    }

private:
    juce::TextEditor editor;
    juce::TextButton clearButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SearchBar)
};

//==============================================================================
//  UI_SPEC_LIBRARY.md: the real LIBRARY view, replacing the M16-T3 scrolling-
//  Label placeholder (its own comment called Collections/Favorites/per-row
//  browsing "honestly deferred rather than half-built" -- this is that later
//  work landing). Reuses the existing importFile path and LibraryManager
//  verbatim (§8: "don't modify"). §9 steps 1-5 (grid, list view toggle,
//  category filter + search, empty states + import, touch pass) are all
//  built here. Deferred: the context menu's assign-to-deck-slot/edit/re-tag
//  items (v1 is Favorite + Delete only) -- flagged in showCardMenu()'s own
//  comment, not silently dropped.
//==============================================================================
class MySamplesTab : public juce::Component,
                     public juce::FileDragAndDropTarget,   // owner: OS files dropped ON the Library import directly
                     private juce::Timer
{
public:
    // Phase 1.1 P2 "New categories: Stems, Loops, Pads, FX, Projects,
    // Templates, Packs" -- widened from the original ALL/LOOPS/PADS/FX v1
    // vocabulary.
    enum class CategoryFilter { all, stems, loops, pads, fx, projects, templates, packs };

    // Phase 1.1 P2 "Sorting by Name/Tempo/Key/Date Added/Category/Duration/
    // Time Signature."
    enum class SortField { name, tempo, key, dateAdded, category, duration, timeSignature };

    // SPEC_PERFORM_V2 GROUP B: set by SessionComponent after construction.
    // Called first on every card tap; if it returns true (a deck slot was
    // armed via its own "+"/empty-tap and just consumed this tap to load
    // into it), the normal audition-toggle below is skipped entirely.
    // Left unset, every tap auditions exactly as before -- zero behavior
    // change until GROUP B's own gesture actually arms a pending target.
    std::function<bool (const juce::String&)> onCardTapMaybeLoad;

    MySamplesTab (ezlibrary::LibraryManager& libraryManagerIn, juce::AudioFormatManager& formatManagerIn,
                  std::function<void (juce::String)> onMessageIn,
                  std::function<void (std::shared_ptr<const std::vector<float>>, std::shared_ptr<const std::vector<float>>, double)> onStartPreviewFn = {},
                  std::function<void()> onStopPreviewFn = {},
                  std::function<bool()> isPreviewActiveFn = {})
        : libraryManager (libraryManagerIn), formatManager (formatManagerIn), onMessage (std::move (onMessageIn)),
          onStartPreview (std::move (onStartPreviewFn)), onStopPreview (std::move (onStopPreviewFn)),
          isPreviewActive (std::move (isPreviewActiveFn))
    {
        titleLabel.setText ("LIBRARY", juce::dontSendNotification);
        // SPEC_PERFORM_V2 GROUP F (105): same kerning treatment as every
        // other uppercase panel/dock title -- see DockHeaderBar's own.
        titleLabel.setFont (performfonts::headingFont (13.0f).withExtraKerningFactor (0.01f));
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));   // kTextBright
        addAndMakeVisible (titleLabel);

        searchBox.setPlaceholder ("Search name, tag, key...");
        searchBox.onTextChange = [this] { refreshCards(); };
        addAndMakeVisible (searchBox);

        // §2 (widened, Phase 1.1 P2): segmented buttons ALL / STEMS / LOOPS /
        // PADS / FX / PROJECTS / TEMPLATES / PACKS (each 44px min hit area,
        // active kIndigo). LibraryEntry's category is a freeform string
        // (§ARCHITECTURE's own "controlled vocabulary... freeform for v1"),
        // so each matches by a case-insensitive substring rather than an
        // exact enum compare -- unchanged reasoning, just more buttons.
        static const struct { CategoryFilter filter; const char* label; } kFilterButtons[] = {
            { CategoryFilter::all,       "ALL" },       { CategoryFilter::stems,     "STEMS" },
            { CategoryFilter::loops,     "LOOPS" },     { CategoryFilter::pads,      "PADS" },
            { CategoryFilter::fx,        "FX" },        { CategoryFilter::projects,  "PROJECTS" },
            { CategoryFilter::templates, "TEMPLATES" }, { CategoryFilter::packs,     "PACKS" }
        };
        int radioGroupId = 1001;
        for (auto& fb : kFilterButtons)
        {
            auto* b = filterButtons.add (new juce::TextButton (fb.label));
            b->setClickingTogglesState (true);
            b->setRadioGroupId (radioGroupId);
            b->setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527u));   // kCard
            b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff7c5cffu));   // kIndigo
            b->setToggleState (fb.filter == CategoryFilter::all, juce::dontSendNotification);
            b->onClick = [this, filter = fb.filter] { categoryFilter = filter; refreshCards(); };
            addAndMakeVisible (b);
        }

        // Phase 1.1 P2 "Sorting by Name/Tempo/Key/Date Added/Category/
        // Duration/Time Signature." Combo box item IDs are 1-based and map
        // directly to this array's own index + 1 -- kSortOrder is `static`
        // (constructor-local but program-lifetime storage), so the onChange
        // lambda below can safely reference it by pointer across every
        // future call, not just this one construction.
        static const SortField kSortOrder[] = {
            SortField::name, SortField::tempo, SortField::key, SortField::dateAdded,
            SortField::category, SortField::duration, SortField::timeSignature
        };
        static const char* kSortLabels[] = {
            "Sort: Name", "Sort: Tempo", "Sort: Key", "Sort: Date Added",
            "Sort: Category", "Sort: Duration", "Sort: Time Signature"
        };
        for (int i = 0; i < (int) juce::numElementsInArray (kSortLabels); ++i)
            sortBox.addItem (kSortLabels[i], i + 1);
        sortBox.setSelectedId (1, juce::dontSendNotification);   // Name
        sortBox.onChange = [this]
        {
            const int idx = sortBox.getSelectedId() - 1;
            if (idx >= 0 && idx < (int) juce::numElementsInArray (kSortOrder)) { sortField = kSortOrder[idx]; refreshCards(); }
        };
        addAndMakeVisible (sortBox);

        // §2: "a view toggle (grid / list icons, 44x44 each, active kIndigo)"
        gridViewButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xa6")));   // grid glyph
        listViewButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x98\xb0")));    // list glyph
        gridViewButton.setRadioGroupId (1002);
        listViewButton.setRadioGroupId (1002);
        for (auto* b : { &gridViewButton, &listViewButton })
        {
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527u));
            b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff7c5cffu));
            addAndMakeVisible (*b);
        }
        gridViewButton.setToggleState (true, juce::dontSendNotification);
        gridViewButton.onClick = [this] { setListView (false); };
        listViewButton.onClick = [this] { setListView (true); };

        importButton.setButtonText ("+ IMPORT");
        importButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7c5cffu));   // kIndigo
        importButton.onClick = [this] { chooseFilesToImport(); };
        addAndMakeVisible (importButton);

        // §5: "an actual button, not a sentence telling them where to click"
        emptyImportButton.setButtonText ("+ Import samples");
        emptyImportButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7c5cffu));
        emptyImportButton.onClick = [this] { chooseFilesToImport(); };
        addChildComponent (emptyImportButton);

        emptyStateLabel.setJustificationType (juce::Justification::centred);
        emptyStateLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));   // kTextDim
        emptyStateLabel.setFont (juce::Font (juce::FontOptions (15.0f)));
        addChildComponent (emptyStateLabel);

        // Folders: "+ FOLDER" in the toolbar, and a bar under the filters that
        // shows the open folder (back / name / options) or, while samples are
        // selected, what to do with them. See updateFolderBar().
        newFolderButton.setButtonText ("+ FOLDER");
        newFolderButton.setTooltip ("New folder - from the selected samples, or empty");
        newFolderButton.onClick = [this] { promptNewFolder (selectedIds); };
        addAndMakeVisible (newFolderButton);

        backButton.setButtonText ("< LIBRARY");
        backButton.onClick = [this] { openFolder ({}); };
        folderOptionsButton.setButtonText ("FOLDER OPTIONS");
        folderOptionsButton.onClick = [this] { showFolderMenu (openFolderId); };
        selectionNewFolderButton.setButtonText ("NEW FOLDER");
        selectionNewFolderButton.onClick = [this] { promptNewFolder (selectedIds); };
        selectionMoveButton.setButtonText ("MOVE TO FOLDER");
        selectionMoveButton.onClick = [this] { showSelectionMoveMenu(); };
        selectionDoneButton.setButtonText ("DONE");
        selectionDoneButton.onClick = [this] { clearSelection(); };
        for (auto* b : { &newFolderButton, &backButton, &folderOptionsButton, &selectionNewFolderButton, &selectionMoveButton, &selectionDoneButton })
        {
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff151527u));   // kCard
            if (b != &newFolderButton) addChildComponent (*b);
        }
        barLabel.setFont (performfonts::headingFont (13.0f).withExtraKerningFactor (0.01f));
        barLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));   // kTextBright
        addChildComponent (barLabel);

        viewport.setViewedComponent (&gridHolder, false);
        viewport.setScrollBarsShown (true, false);
        addChildComponent (viewport);

        // roadmap "tap-to-audition, one at a time": reflect REAL preview
        // state on every card, same "poll real state, never fake it"
        // convention as ClipEditorContent's own preview-button timer.
        startTimerHz (15);

        refreshCards();
    }

    ~MySamplesTab() override { if (onStopPreview) onStopPreview(); }

    void resized() override
    {
        auto area = getLocalBounds();

        // touch pass (§7): every toolbar control gets a real 44px interior
        // height (52px outer reduced by 4 top/bottom), matching the
        // convention already established across this app's other toolbars.
        auto toolbar = area.removeFromTop (52).reduced (12, 4);
        titleLabel.setBounds (toolbar.removeFromLeft (70));
        importButton.setBounds (toolbar.removeFromRight (90));
        toolbar.removeFromRight (8);
        newFolderButton.setBounds (toolbar.removeFromRight (96));
        toolbar.removeFromRight (8);
        listViewButton.setBounds (toolbar.removeFromRight (44));
        toolbar.removeFromRight (4);
        gridViewButton.setBounds (toolbar.removeFromRight (44));
        toolbar.removeFromRight (8);
        searchBox.setBounds (toolbar.removeFromRight (juce::jmax (240, toolbar.getWidth() / 2)));
        toolbar.removeFromRight (8);

        // Phase 1.1 P2 "spacing": the category row moved to its OWN second
        // 44px toolbar row (was crammed into row 1 alongside title/search/
        // view-toggle/import) -- widening ALL/LOOPS/PADS/FX to the full
        // 7-category vocabulary plus adding a sort dropdown made that one
        // row too crowded to give any of them real breathing room.
        auto filterRow = area.removeFromTop (44).reduced (12, 4);

        // "fix: the word 'Loops' is clipped -- fix all typography clipping
        // throughout Library": a fixed 50px per button was narrower than
        // "LOOPS" actually needs at TextButton's own LookAndFeel-provided
        // font, so JUCE truncated it. Sized to each button's own text now
        // (the exact font its LookAndFeel will paint it with, at this row's
        // own 44px control height), matching the header nav tabs' own "size
        // to text, don't guess a fixed width" fix from Phase 1.1 P1.
        // Sorting by Name/Tempo/Key/Date Added/Category/Duration/Time
        // Signature": sortBox anchored to this same row's right edge.
        sortBox.setBounds (filterRow.removeFromRight (160));
        filterRow.removeFromRight (8);

        int filterTotalW = 0;
        for (auto* b : filterButtons)
        {
            const auto f = b->getLookAndFeel().getTextButtonFont (*b, 44);
            filterTotalW += juce::GlyphArrangement::getStringWidthInt (f, b->getButtonText()) + 24;
        }
        filterTotalW += (filterButtons.size() - 1) * 4;

        auto filterArea = filterRow.removeFromLeft (juce::jmin (filterRow.getWidth(), filterTotalW));
        for (auto* b : filterButtons)
        {
            const auto f = b->getLookAndFeel().getTextButtonFont (*b, 44);
            const int w = juce::GlyphArrangement::getStringWidthInt (f, b->getButtonText()) + 24;
            b->setBounds (filterArea.removeFromLeft (w));
            filterArea.removeFromLeft (4);
        }

        if (selectMode || openFolderId.isNotEmpty())
        {
            auto bar = area.removeFromTop (44).reduced (12, 4);
            if (selectMode)
            {
                selectionDoneButton.setBounds (bar.removeFromRight (80));
                bar.removeFromRight (6);
                selectionMoveButton.setBounds (bar.removeFromRight (150));
                bar.removeFromRight (6);
                selectionNewFolderButton.setBounds (bar.removeFromRight (120));
                bar.removeFromRight (10);
            }
            else
            {
                backButton.setBounds (bar.removeFromLeft (110));
                bar.removeFromLeft (12);
                folderOptionsButton.setBounds (bar.removeFromRight (150));
                bar.removeFromRight (10);
            }
            barLabel.setBounds (bar);
        }

        browseArea = area.reduced (12);
        viewport.setBounds (browseArea);

        auto emptyArea = browseArea.withSizeKeepingCentre (juce::jmin (360, browseArea.getWidth()), 100);
        emptyStateLabel.setBounds (emptyArea.removeFromTop (40));
        emptyImportButton.setBounds (emptyArea.withSizeKeepingCentre (160, 44));

        layoutGrid();
    }

private:
    void timerCallback() override
    {
        const bool anyPlaying = isPreviewActive && isPreviewActive();
        for (auto* card : cards)
            card->setAuditioning (anyPlaying && card->getAssetId() == currentlyAuditioningAssetId);
        if (! anyPlaying) currentlyAuditioningAssetId = {};
    }

    // UI_SPEC_LIBRARY.md §2: the view toggle -- swaps every card's own
    // LayoutMode and re-lays out; the SAME LibrarySampleCard instances
    // (rebuilt on the next refreshCards(), simplest correct approach here)
    // handle both, so this is a pure layout change, not a second browser.
    void setListView (bool listMode)
    {
        if (listViewActive == listMode) return;
        listViewActive = listMode;
        refreshCards();
    }

    // UI_SPEC_LIBRARY.md §3/§4: grid is "min(200, availableWidth /
    // floor(width/200))" per card, wrapping rows; list is one 44px row per
    // sample, alternating fill.
    void layoutGrid()
    {
        const int availableWidth = juce::jmax (1, viewport.getWidth());

        const int count = (int) tiles.size();   // folders first, then samples

        if (listViewActive)
        {
            constexpr int rowHeight = 44;
            for (int i = 0; i < count; ++i)
                tiles[(size_t) i]->setBounds (0, i * rowHeight, availableWidth, rowHeight);
            gridHolder.setSize (availableWidth, juce::jmax (viewport.getHeight(), count * rowHeight));
            return;
        }

        const int columns   = juce::jmax (1, availableWidth / 200);
        const int cardWidth = juce::jmin (200, availableWidth / columns);
        constexpr int cardHeight = 140;
        constexpr int gap        = 12;

        for (int i = 0; i < count; ++i)
        {
            const int col = i % columns;
            const int row = i / columns;
            tiles[(size_t) i]->setBounds (col * (cardWidth + gap), row * (cardHeight + gap), cardWidth, cardHeight);
        }

        const int rows = (count + columns - 1) / columns;
        gridHolder.setSize (juce::jmax (viewport.getWidth(), columns * (cardWidth + gap)),
                             juce::jmax (viewport.getHeight(), rows * (cardHeight + gap)));
    }

    // §2's ALL/LOOPS/PADS/FX segmented filter -- see the constructor's own
    // comment on why this is a substring match against a freeform field
    // rather than an exact enum compare.
    bool passesCategoryFilter (const ezlibrary::LibraryEntry& entry) const
    {
        switch (categoryFilter)
        {
            case CategoryFilter::stems:     return entry.category.containsIgnoreCase ("stem");
            case CategoryFilter::loops:     return entry.category.containsIgnoreCase ("loop");
            case CategoryFilter::pads:      return entry.category.containsIgnoreCase ("pad");
            case CategoryFilter::fx:        return entry.category.containsIgnoreCase ("fx");
            case CategoryFilter::projects:  return entry.category.containsIgnoreCase ("project");
            case CategoryFilter::templates: return entry.category.containsIgnoreCase ("template");
            case CategoryFilter::packs:     return entry.category.containsIgnoreCase ("pack");
            case CategoryFilter::all:
            default:                        return true;
        }
    }

    void refreshCards()
    {
        const juce::String filter = searchBox.getText().trim().toLowerCase();
        const bool searching = filter.isNotEmpty();

        // Folders: the top level shows each folder as one card and hides its
        // members; inside a folder only its members show; a search looks
        // through everything, folder contents included.
        if (openFolderId.isNotEmpty() && libraryOwningFolder (openFolderId) == nullptr) openFolderId = {};
        const bool inFolder = openFolderId.isNotEmpty();

        bool anyEntries = false;
        std::map<juce::String, int> folderTotals, folderMatches;
        std::map<juce::String, juce::String> folderAnyMember;
        std::vector<const ezlibrary::LibraryEntry*> matched;
        for (auto& lib : libraryManager.libraries())
            for (auto& entry : lib->entries())
            {
                anyEntries = true;
                const bool inSomeFolder = entry.collectionId.isNotEmpty() && lib->findFolder (entry.collectionId).has_value();
                if (inSomeFolder)
                {
                    ++folderTotals[entry.collectionId];
                    if (folderAnyMember[entry.collectionId].isEmpty()) folderAnyMember[entry.collectionId] = entry.assetId;
                }
                if (! passesCategoryFilter (entry)) continue;
                if (inSomeFolder) ++folderMatches[entry.collectionId];
                if (inFolder ? entry.collectionId != openFolderId : (! searching && inSomeFolder)) continue;
                if (searching
                    && ! entry.name.toLowerCase().contains (filter)
                    && ! entry.category.toLowerCase().contains (filter)
                    && ! entry.tags.toLowerCase().contains (filter))
                    continue;
                matched.push_back (&entry);
            }

        std::vector<std::pair<const ezlibrary::LibraryFolder*, const ezlibrary::Library*>> shownFolders;
        if (! inFolder)
            for (auto& lib : libraryManager.libraries())
                for (auto& folder : lib->folders())
                {
                    anyEntries = true;
                    if (searching ? ! folder.name.toLowerCase().contains (filter)
                                  : (categoryFilter != CategoryFilter::all && folderMatches[folder.id] == 0))
                        continue;
                    shownFolders.push_back ({ &folder, lib.get() });
                }
        std::stable_sort (shownFolders.begin(), shownFolders.end(), [] (const auto& a, const auto& b)
                          { return a.first->name.compareNatural (b.first->name) < 0; });

        // Phase 1.1 P2 "Sorting by Name/Tempo/Key/Date Added/Category/
        // Duration/Time Signature." stable_sort so entries that compare
        // equal (e.g. two untagged-key samples) keep their existing
        // relative order rather than shuffling on every refresh.
        std::stable_sort (matched.begin(), matched.end(), [this] (const ezlibrary::LibraryEntry* a, const ezlibrary::LibraryEntry* b)
        {
            switch (sortField)
            {
                case SortField::name:          return a->name.compareIgnoreCase (b->name) < 0;
                case SortField::tempo:         return a->detectedBpm < b->detectedBpm;
                case SortField::key:           return a->key.compareIgnoreCase (b->key) < 0;
                case SortField::dateAdded:     return a->importedAtMs < b->importedAtMs;
                case SortField::category:      return a->category.compareIgnoreCase (b->category) < 0;
                case SortField::timeSignature: return a->timeSignature.compareIgnoreCase (b->timeSignature) < 0;
                case SortField::duration:      return durationFor (a->assetId) < durationFor (b->assetId);
                default:                       return false;
            }
        });

        const bool showEmpty = matched.empty() && shownFolders.empty();
        emptyStateLabel.setVisible (showEmpty);
        emptyImportButton.setVisible (showEmpty && ! anyEntries);   // §5: the prominent import button only for a truly empty library, not a search miss
        viewport.setVisible (! showEmpty);

        if (showEmpty)
        {
            juce::String text;
            if (! anyEntries)   text = "No samples yet";
            else if (searching) text = juce::String (inFolder ? "Nothing in this folder matches \"" : "No samples match \"") + searchBox.getText() + "\"";
            else if (inFolder)  text = "This folder is empty - drag samples onto it";
            else                text = "Nothing in this category";
            emptyStateLabel.setText (text, juce::dontSendNotification);
        }

        tiles.clear();
        cards.clear();
        folderCards.clear();

        for (auto& shown : shownFolders)
        {
            const auto& folder = *shown.first;
            auto* folderCard = folderCards.add (new LibraryFolderCard());
            folderCard->setFolder (folder.name, folderTotals[folder.id],
                                   LibrarySampleCard::collectionColorFor (folder.id), folderPicture (*shown.second, folder.id),
                                   folderAnyMember[folder.id]);
            folderCard->setListMode (listViewActive);
            gridHolder.addAndMakeVisible (folderCard);
            tiles.push_back (folderCard);

            const juce::String folderId = folder.id;
            // deferred: each of these rebuilds the cards, the calling one included
            folderCard->onOpen = [this, folderId] { later ([this, folderId] { openFolder (folderId); }); };
            folderCard->onMenu = [this, folderId] { later ([this, folderId] { showFolderMenu (folderId); }); };
            folderCard->onAssetDropped = [this, folderId] (const juce::String& droppedId)
            {
                // dragging one card of a selection moves the whole selection
                if (auto* owner = libraryOwningAsset (droppedId))
                    if (owner->findById (droppedId)->collectionId == folderId) return;   // already in it (e.g. the folder dropped on itself)
                const juce::StringArray ids = selectedIds.contains (droppedId) ? selectedIds : juce::StringArray (droppedId);
                later ([this, ids, folderId] { moveAssetsToFolder (ids, folderId); });
            };
        }

        int index = 0;
        for (auto* entryPtr : matched)
        {
            auto* card = cards.add (new LibrarySampleCard());
            card->setEntry (*entryPtr);
            card->setLayoutMode (listViewActive ? LibrarySampleCard::LayoutMode::list : LibrarySampleCard::LayoutMode::grid);
            card->setAlternateShade ((index++ % 2) == 1);
            card->setSelected (selectedIds.contains (entryPtr->assetId));
            gridHolder.addAndMakeVisible (card);
            tiles.push_back (card);

            const juce::String assetId = entryPtr->assetId;
            auto loaded = getOrDecodeSamples (assetId);
            card->setAudio (*loaded.samples);
            if (loaded.sampleRate > 0.0) card->setDurationSeconds ((double) loaded.samples->size() / loaded.sampleRate);
            card->setAuditioning (isPreviewActive && isPreviewActive() && assetId == currentlyAuditioningAssetId);

            card->onTap  = [this, assetId]
            {
                if (selectMode) { toggleSelected (assetId); return; }
                if (onCardTapMaybeLoad != nullptr && onCardTapMaybeLoad (assetId)) return;
                toggleAudition (assetId);
            };
            card->onToggleSelect = [this, assetId] { toggleSelected (assetId); };
            card->onMenu = [this, assetId] { showCardMenu (assetId); };
        }

        updateFolderBar();   // re-lays out the grid too
    }

    // Decodes once per assetId, ever (cached for the tab's lifetime) -- the
    // SAME data backs both the card's waveform (§3: "compute peaks once on
    // load, never in paint") and audition playback, so there's exactly one
    // decode per sample, not two. HONEST GAP: this decodes synchronously on
    // first view, on the message thread -- fine for a personal library of
    // dozens-to-low-hundreds of samples, but would visibly stall the UI for
    // a very large one. A background/async loader (MILESTONE_16_ARCHITECTURE.md
    // already anticipates a juce::ThreadPool for batch import) would be the
    // real fix -- not built here, flagged rather than silently accepted.
    struct LoadedSample
    {
        std::shared_ptr<const std::vector<float>> samples;
        double sampleRate { 0.0 };   // needed for the list view's duration column; 0 = unknown (decode failed)
    };

    LoadedSample getOrDecodeSamples (const juce::String& assetId)
    {
        auto it = sampleCache.find (assetId);
        if (it != sampleCache.end()) return it->second;

        LoadedSample loaded;
        auto samples = std::make_shared<std::vector<float>>();
        auto file = libraryManager.resolve (assetId);
        if (file.existsAsFile())
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
            // SECURITY: this runs for EVERY visible library entry on every
            // refresh, so one poisoned entry would otherwise make the
            // Library tab permanently un-openable (16 GB alloc / crash).
            juce::String geometryReason;
            if (reader != nullptr && eximport::readerGeometryIsSane (*reader, geometryReason))
            {
                const int len = (int) reader->lengthInSamples;

                // PX-A, last of the three load paths. This is a MONO preview
                // buffer, so only the reader's left channel is wanted -- the
                // old code still decoded every channel of the file into a
                // scratch buffer and then copied one of them out. Now it reads
                // the one channel it needs straight into the cache entry.
                //
                // Guarded on len: a zero-length entry would leave data() null,
                // and AudioBuffer requires a real pointer. This runs for every
                // visible library row, so one bad file must not take the tab
                // down -- the same reasoning as the geometry gate above.
                if (len > 0)
                {
                    samples->resize ((size_t) len);
                    float* chans[1] { samples->data() };
                    juce::AudioBuffer<float> dest (chans, 1, len);
                    reader->read (&dest, 0, len, 0, true, false);
                }
                loaded.sampleRate = reader->sampleRate;
            }
        }
        loaded.samples = samples;
        sampleCache[assetId] = loaded;
        return loaded;
    }

    // Phase 1.1 P2 "Sorting... by Duration" -- reuses the exact same decode
    // cache getOrDecodeSamples() already maintains (refreshCards() decodes
    // every visible entry anyway to build its waveform, so sorting by
    // duration costs nothing beyond what already happens, not a second pass
    // over the audio).
    double durationFor (const juce::String& assetId)
    {
        auto loaded = getOrDecodeSamples (assetId);
        return loaded.sampleRate > 0.0 ? (double) loaded.samples->size() / loaded.sampleRate : 0.0;
    }

    // §3: "Tap -> audition (play/stop preview, one at a time, globally
    // exclusive)". Routes through SessionComponent's LibraryPreviewVoice
    // (threaded in via onStartPreview/onStopPreview) rather than a private
    // playback path -- true single-preview exclusivity across the whole
    // app, not just within this tab.
    void toggleAudition (const juce::String& assetId)
    {
        const bool alreadyThis = (assetId == currentlyAuditioningAssetId) && isPreviewActive && isPreviewActive();
        if (alreadyThis)
        {
            if (onStopPreview) onStopPreview();
            currentlyAuditioningAssetId = {};
            return;
        }

        auto loaded = getOrDecodeSamples (assetId);
        if (loaded.samples->empty())
        {
            if (onMessage) onMessage ("Could not load this sample for preview");
            return;
        }
        // Third argument is the FILE's sample rate (this tab doesn't know
        // the device rate); SessionComponent's wiring converts it to the
        // playback rate ratio. Was a hardcoded 1.0 -- the same
        // pitched-sharp bug the pads had, in the audition path.
        if (onStartPreview) onStartPreview (loaded.samples, nullptr, loaded.sampleRate);   // mono -- renderLibraryPreview() plays the same buffer to both channels when right is null
        currentlyAuditioningAssetId = assetId;
    }

    // §3's full menu is "assign to a deck slot, edit, delete, re-tag" --
    // v1 here is Favorite toggle + Delete only. Assign-to-deck-slot/edit/
    // re-tag are deferred (would need a deck-slot picker and rename dialog
    // threaded in from SessionComponent -- real, buildable, just not this
    // pass), not silently omitted.
    void showCardMenu (const juce::String& assetId)
    {
        ezlibrary::Library* owningLib = nullptr;
        ezlibrary::LibraryEntry entry;
        for (auto& lib : libraryManager.libraries())
            if (auto e = lib->findById (assetId)) { owningLib = lib.get(); entry = *e; break; }
        if (owningLib == nullptr) return;

        const bool inFolder = entry.collectionId.isNotEmpty() && owningLib->findFolder (entry.collectionId).has_value();
        const bool isSelected = selectedIds.contains (assetId);
        // folder moves act on the whole selection when this card is part of it
        const juce::StringArray moveIds = isSelected ? selectedIds : juce::StringArray (assetId);

        juce::PopupMenu menu;
        menu.addItem (1, entry.favorite ? "Remove from Favorites" : "Add to Favorites");
        menu.addItem (3, "Edit Metadata...");
        menu.addSeparator();
        menu.addSubMenu (moveIds.size() > 1 ? "Move " + juce::String (moveIds.size()) + " Selected to Folder" : juce::String ("Move to Folder"),
                         buildMoveMenu (*owningLib, entry.collectionId));
        if (inFolder) menu.addItem (21, moveIds.size() > 1 ? "Remove Selected from Folder" : "Remove from Folder");
        menu.addItem (10, isSelected ? "Deselect" : "Select");
        menu.addSeparator();
        menu.addItem (2, "Delete from Library");

        menu.showMenuAsync (juce::PopupMenu::Options(), [this, assetId, owningLib, moveIds] (int result)
        {
            if (handleMoveResult (result, *owningLib, moveIds)) return;
            if (result == 10) { toggleSelected (assetId); return; }
            if (result == 1)
            {
                if (auto e = owningLib->findById (assetId))
                {
                    auto updated = *e;
                    updated.favorite = ! updated.favorite;
                    owningLib->upsert (updated);
                    owningLib->save();
                    refreshCards();
                }
            }
            else if (result == 2)
            {
                // Library::remove() is pure bookkeeping (matches upsert()'s
                // own documented scope) -- removes the catalogue entry, does
                // NOT delete the copied file from Samples/ on disk. Matching
                // that existing scope deliberately: actual file deletion is
                // a separate, more destructive action this menu doesn't take.
                owningLib->remove (assetId);
                owningLib->save();
                if (onMessage) onMessage ("Removed from library");
                refreshCards();
            }
            else if (result == 3)
            {
                promptEditAssetMetadata (assetId, owningLib);
            }
        });
    }

    // Phase 1.1 P2 "Metadata editing for every asset: Name, Artist, Album,
    // Tags, Category, Key, Tempo, Time Signature, Genre, Notes, Creator,
    // Rating (dropdowns where appropriate)." One juce::AlertWindow with
    // every field, matching this whole codebase's established "AlertWindow
    // for all modal editing" convention (promptSetDeckTempo/promptRenameRow/
    // promptRowNotes/promptRowTags all do the same) rather than introducing
    // a second dialog framework for just this one screen. Category/Key/
    // Time Signature/Rating are combo boxes ("dropdowns where appropriate");
    // Name/Artist/Album/Tags/Tempo/Genre/Creator/Notes are free text, since
    // none of those have (or should have) a fixed vocabulary.
    // onFinished (optional): called after the dialog closes (saved OR
    // cancelled) -- lets the import flow below walk this editor through
    // several freshly-imported files one after another.
    void promptEditAssetMetadata (const juce::String& assetId, ezlibrary::Library* owningLib,
                                   std::function<void()> onFinished = {})
    {
        auto existing = owningLib->findById (assetId);
        if (! existing.has_value()) { if (onFinished) onFinished(); return; }
        const auto& e = *existing;

        auto* aw = new juce::AlertWindow ("Edit Metadata", e.name, juce::MessageBoxIconType::NoIcon);

        aw->addTextEditor ("name", e.name, "Name");
        aw->addTextEditor ("artist", e.artist, "Artist");
        aw->addTextEditor ("album", e.album, "Album");
        aw->addTextEditor ("tags", e.tags, "Tags (comma-separated)");

        static const char* kCategories[] = { "Stems", "Loops", "Pads", "FX", "Projects", "Templates", "Packs" };
        juce::StringArray categoryOptions (kCategories, (int) juce::numElementsInArray (kCategories));
        int categoryIndex = categoryOptions.indexOf (e.category, true) + 1;   // 1-based (0 = none selected); +1 lands on the right item, or 0 ("(none)") if not found
        categoryOptions.insert (0, "(none)");
        aw->addComboBox ("category", categoryOptions, "Category");
        aw->getComboBoxComponent ("category")->setSelectedItemIndex (categoryIndex, juce::dontSendNotification);

        static const char* kKeys[] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
            "Cm", "C#m", "Dm", "D#m", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "A#m", "Bm"
        };
        juce::StringArray keyOptions (kKeys, (int) juce::numElementsInArray (kKeys));
        int keyIndex = keyOptions.indexOf (e.key, true) + 1;
        keyOptions.insert (0, "(none)");
        aw->addComboBox ("key", keyOptions, "Key");
        aw->getComboBoxComponent ("key")->setSelectedItemIndex (keyIndex, juce::dontSendNotification);

        aw->addTextEditor ("tempo", e.detectedBpm > 0.0 ? juce::String (e.detectedBpm, 1) : juce::String(), "Tempo (BPM)");

        static const char* kTimeSigs[] = { "4/4", "3/4", "2/4", "6/8", "5/4", "7/8" };
        juce::StringArray timeSigOptions (kTimeSigs, (int) juce::numElementsInArray (kTimeSigs));
        int timeSigIndex = timeSigOptions.indexOf (e.timeSignature, true) + 1;
        timeSigOptions.insert (0, "(none)");
        aw->addComboBox ("timeSignature", timeSigOptions, "Time Signature");
        aw->getComboBoxComponent ("timeSignature")->setSelectedItemIndex (timeSigIndex, juce::dontSendNotification);

        aw->addTextEditor ("genre", e.genre, "Genre");
        aw->addTextEditor ("creator", e.creator, "Creator");
        aw->addTextEditor ("notes", e.notes, "Notes");
        aw->getTextEditor ("notes")->setMultiLine (true);

        juce::StringArray ratingOptions { "(none)", "1", "2", "3", "4", "5" };
        aw->addComboBox ("rating", ratingOptions, "Rating");
        aw->getComboBoxComponent ("rating")->setSelectedItemIndex (juce::jlimit (0, 5, e.rating), juce::dontSendNotification);

        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, assetId, owningLib, onFinished] (int result)
            {
                bool needsCategory = false;   // owner: "Category should be required" -- see below
                if (result == 1)
                {
                    if (auto current = owningLib->findById (assetId))
                    {
                        auto updated = *current;
                        updated.name    = aw->getTextEditorContents ("name").trim();
                        updated.artist  = aw->getTextEditorContents ("artist").trim();
                        updated.album   = aw->getTextEditorContents ("album").trim();
                        updated.tags    = aw->getTextEditorContents ("tags").trim();
                        updated.genre   = aw->getTextEditorContents ("genre").trim();
                        updated.creator = aw->getTextEditorContents ("creator").trim();
                        updated.notes   = aw->getTextEditorContents ("notes");

                        const juce::String categoryText = aw->getComboBoxComponent ("category")->getText();
                        updated.category = categoryText == "(none)" ? juce::String() : categoryText;
                        const juce::String keyText = aw->getComboBoxComponent ("key")->getText();
                        updated.key = keyText == "(none)" ? juce::String() : keyText;
                        const juce::String timeSigText = aw->getComboBoxComponent ("timeSignature")->getText();
                        updated.timeSignature = timeSigText == "(none)" ? juce::String() : timeSigText;
                        const juce::String ratingText = aw->getComboBoxComponent ("rating")->getText();
                        updated.rating = ratingText == "(none)" ? 0 : ratingText.getIntValue();

                        const double tempo = aw->getTextEditorContents ("tempo").getDoubleValue();
                        if (tempo > 0.0) updated.detectedBpm = tempo;

                        owningLib->upsert (updated);
                        owningLib->save();
                        refreshCards();

                        // Owner: "Category should be required." Everything
                        // else the user typed is already saved above, so
                        // nothing is lost -- the editor simply reopens
                        // until a category is actually chosen. Cancel still
                        // abandons the edit entirely (required-to-SAVE, not
                        // impossible-to-leave).
                        needsCategory = updated.category.isEmpty();
                        if (onMessage) onMessage (needsCategory ? juce::String ("Category is required - please choose one")
                                                                 : updated.name + ": metadata saved");
                    }
                }
                delete aw;
                if (needsCategory) { promptEditAssetMetadata (assetId, owningLib, onFinished); return; }
                if (onFinished) onFinished();
            }), false);
    }

    // Owner request: "when uploading files to the library give me the
    // option to insert the metadata." Walks the (existing) metadata editor
    // through each just-imported asset in turn -- next dialog opens when
    // the previous one closes, saved or cancelled.
    void editMetadataSequentially (juce::StringArray assetIds, int index, ezlibrary::Library* owningLib)
    {
        if (index >= assetIds.size()) return;
        promptEditAssetMetadata (assetIds[index], owningLib,
            [this, assetIds, index, owningLib] { editMetadataSequentially (assetIds, index + 1, owningLib); });
    }

    // ---- Folders -------------------------------------------------------------

    void later (std::function<void()> fn)
    {
        juce::Component::SafePointer<MySamplesTab> safe (this);
        juce::MessageManager::callAsync ([safe, fn] { if (safe != nullptr) fn(); });
    }

    ezlibrary::Library* libraryOwningFolder (const juce::String& folderId) const
    {
        for (auto& lib : libraryManager.libraries())
            if (lib->findFolder (folderId).has_value()) return lib.get();
        return nullptr;
    }

    ezlibrary::Library* libraryOwningAsset (const juce::String& assetId) const
    {
        for (auto& lib : libraryManager.libraries())
            if (lib->findById (assetId).has_value()) return lib.get();
        return nullptr;
    }

    static std::vector<ezlibrary::LibraryFolder> sortedFolders (const ezlibrary::Library& lib)
    {
        auto list = lib.folders();
        std::stable_sort (list.begin(), list.end(), [] (const ezlibrary::LibraryFolder& a, const ezlibrary::LibraryFolder& b)
                          { return a.name.compareNatural (b.name) < 0; });
        return list;
    }

    void openFolder (const juce::String& folderId)
    {
        openFolderId = folderId;
        selectedIds.clear();
        selectMode = false;
        refreshCards();
        viewport.setViewPosition (0, 0);
    }

    void toggleSelected (const juce::String& assetId)
    {
        if (selectedIds.contains (assetId)) selectedIds.removeString (assetId);
        else                                selectedIds.add (assetId);
        selectMode = ! selectedIds.isEmpty();
        for (auto* c : cards) c->setSelected (selectedIds.contains (c->getAssetId()));
        updateFolderBar();
    }

    void clearSelection()
    {
        selectedIds.clear();
        selectMode = false;
        for (auto* c : cards) c->setSelected (false);
        updateFolderBar();
    }

    // The bar under the filters: while samples are selected, what to do with
    // them; otherwise, inside a folder, back / the folder's name / options.
    void updateFolderBar()
    {
        const bool inFolder = openFolderId.isNotEmpty();
        for (auto* b : { &selectionNewFolderButton, &selectionMoveButton, &selectionDoneButton }) b->setVisible (selectMode);
        backButton.setVisible (inFolder && ! selectMode);
        folderOptionsButton.setVisible (inFolder && ! selectMode);
        barLabel.setVisible (selectMode || inFolder);

        if (selectMode)
        {
            barLabel.setText (juce::String (selectedIds.size()) + " SELECTED", juce::dontSendNotification);
        }
        else if (auto* lib = inFolder ? libraryOwningFolder (openFolderId) : nullptr)
        {
            const int n = lib->folderSize (openFolderId);
            barLabel.setText (lib->findFolder (openFolderId)->name.toUpperCase() + "     " + juce::String (n) + (n == 1 ? " ITEM" : " ITEMS"),
                              juce::dontSendNotification);
        }
        resized();
    }

    // Decoded once per picture file; a new picture always gets a new file name.
    juce::Image folderPicture (const ezlibrary::Library& lib, const juce::String& folderId)
    {
        const auto file = lib.folderImageFile (folderId);
        if (! file.existsAsFile()) return {};
        const auto key = file.getFullPathName();
        if (auto it = folderImages.find (key); it != folderImages.end()) return it->second;

        juce::Image image;
        if (file.getSize() < 8 * 1024 * 1024)   // the app writes 512 px PNGs; anything far bigger was not written by it
            image = juce::ImageFileFormat::loadFrom (file);
        folderImages[key] = image;
        return image;
    }

    // folderId empty = take the samples out of whatever folder they're in.
    void moveAssetsToFolder (const juce::StringArray& assetIds, const juce::String& folderId)
    {
        int moved = 0;
        for (auto& lib : libraryManager.libraries())
        {
            if (folderId.isNotEmpty() && ! lib->findFolder (folderId).has_value()) continue;
            bool changed = false;
            for (auto& id : assetIds)
                if (lib->setEntryFolder (id, folderId)) { ++moved; changed = true; }
            if (changed) lib->save();
        }

        selectedIds.clear();
        selectMode = false;
        refreshCards();

        if (onMessage == nullptr || moved == 0) return;
        const juce::String what = moved == 1 ? juce::String ("1 sample") : juce::String (moved) + " samples";
        if (folderId.isEmpty())
            onMessage (what + " taken out of the folder");
        else if (auto* lib = libraryOwningFolder (folderId))
            onMessage (what + " moved to \"" + lib->findFolder (folderId)->name + "\"");
    }

    // Item ids: 20 New Folder..., 21 Remove from Folder, 100 + n = the n-th of sortedFolders().
    juce::PopupMenu buildMoveMenu (const ezlibrary::Library& lib, const juce::String& currentFolderId) const
    {
        juce::PopupMenu m;
        m.addItem (20, "New Folder...");
        const auto folders = sortedFolders (lib);
        if (! folders.empty()) m.addSeparator();
        for (int i = 0; i < (int) folders.size(); ++i)
        {
            const auto& f = folders[(size_t) i];
            m.addItem (100 + i, f.name, f.id != currentFolderId, f.id == currentFolderId);
        }
        return m;
    }

    bool handleMoveResult (int result, const ezlibrary::Library& lib, const juce::StringArray& assetIds)
    {
        if (result == 20) { promptNewFolder (assetIds); return true; }
        if (result == 21) { moveAssetsToFolder (assetIds, {}); return true; }
        if (result >= 100)
        {
            const auto folders = sortedFolders (lib);
            const int i = result - 100;
            if (i < (int) folders.size()) moveAssetsToFolder (assetIds, folders[(size_t) i].id);
            return true;
        }
        return false;
    }

    void showSelectionMoveMenu()
    {
        if (selectedIds.isEmpty()) return;
        auto* lib = libraryOwningAsset (selectedIds[0]);
        if (lib == nullptr) return;
        auto m = buildMoveMenu (*lib, openFolderId);
        if (openFolderId.isNotEmpty()) { m.addSeparator(); m.addItem (21, "Remove from This Folder"); }
        const auto ids = selectedIds;
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&selectionMoveButton),
                         [this, lib, ids] (int result) { handleMoveResult (result, *lib, ids); });
    }

    // New folder holding assetIds -- empty makes an empty folder to drag into.
    void promptNewFolder (const juce::StringArray& assetIds)
    {
        ezlibrary::Library* lib = assetIds.isEmpty() ? nullptr : libraryOwningAsset (assetIds[0]);
        if (lib == nullptr && ! libraryManager.libraries().empty()) lib = libraryManager.libraries().front().get();
        if (lib == nullptr) return;

        juce::StringArray names;
        for (auto& id : assetIds) if (auto e = lib->findById (id)) names.add (e->name);

        const juce::String message = assetIds.isEmpty()
            ? juce::String ("Name the folder, then drag samples onto it.")
            : "Put the " + juce::String (assetIds.size()) + (assetIds.size() == 1 ? " selected sample" : " selected samples") + " in a new folder.";
        auto* aw = new juce::AlertWindow ("New Folder", message, juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", names.isEmpty() ? juce::String ("New folder") : ezlibrary::Library::suggestFolderName (names), "Folder name");
        aw->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Create + Picture...", 2);
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw, assetIds, lib] (int result)
        {
            const juce::String name = aw->getTextEditorContents ("name");
            delete aw;
            if (result == 0) return;

            const auto folder = lib->createFolder (name, assetIds);
            lib->save();
            selectedIds.clear();
            selectMode = false;
            refreshCards();
            if (onMessage) onMessage ("Created folder \"" + folder.name + "\"");
            if (result == 2) choosePictureForFolder (folder.id, {});
        }), false);
    }

    void promptRenameFolder (const juce::String& folderId)
    {
        auto* lib = libraryOwningFolder (folderId);
        if (lib == nullptr) return;
        auto* aw = new juce::AlertWindow ("Rename Folder", juce::String(), juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", lib->findFolder (folderId)->name, "Folder name");
        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw, folderId] (int result)
        {
            const juce::String name = aw->getTextEditorContents ("name").trim();
            delete aw;
            if (result != 1 || name.isEmpty()) return;
            if (auto* owner = libraryOwningFolder (folderId))
            {
                auto updated = *owner->findFolder (folderId);
                updated.name = name.substring (0, ezlibrary::Library::kMaxFolderNameLength);
                owner->upsertFolder (updated);
                owner->save();
                refreshCards();
            }
        }), false);
    }

    void showFolderMenu (const juce::String& folderId)
    {
        auto* lib = libraryOwningFolder (folderId);
        if (lib == nullptr) return;
        const bool hasPicture = lib->folderImageFile (folderId).existsAsFile();

        juce::PopupMenu m;
        if (openFolderId != folderId) m.addItem (1, "Open");
        m.addItem (2, "Rename...");
        m.addItem (3, hasPicture ? "Change Picture..." : "Choose Picture...");
        if (hasPicture) m.addItem (4, "Remove Picture");
        m.addSeparator();
        m.addItem (5, "Delete Folder (keeps the samples)");

        auto options = juce::PopupMenu::Options();
        if (openFolderId == folderId) options = options.withTargetComponent (&folderOptionsButton);
        m.showMenuAsync (options, [this, folderId] (int result)
        {
            auto* owner = libraryOwningFolder (folderId);
            if (owner == nullptr) return;
            if (result == 1) openFolder (folderId);
            else if (result == 2) promptRenameFolder (folderId);
            else if (result == 3) choosePictureForFolder (folderId, {});
            else if (result == 4) { owner->clearFolderImage (folderId); owner->save(); refreshCards(); }
            else if (result == 5)
            {
                const int n = owner->folderSize (folderId);
                const juce::String name = owner->findFolder (folderId)->name;
                owner->removeFolder (folderId);
                owner->save();
                if (openFolderId == folderId) openFolderId = {};
                refreshCards();
                if (onMessage) onMessage ("Deleted folder \"" + name + "\" - its " + juce::String (n)
                                            + (n == 1 ? " sample is" : " samples are") + " still in the library");
            }
        });
    }

    void choosePictureForFolder (const juce::String& folderId, std::function<void()> onDone)
    {
        pictureChooser = std::make_unique<juce::FileChooser> ("Choose a picture for the folder...",
                                                                juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
                                                                "*.png;*.jpg;*.jpeg");
        constexpr auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        pictureChooser->launchAsync (chooserFlags, [this, folderId, onDone] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file != juce::File())
            {
                if (auto* lib = libraryOwningFolder (folderId))
                {
                    juce::String error;
                    if (lib->setFolderImage (folderId, file, error))
                    {
                        lib->save();
                        refreshCards();
                        if (onMessage) onMessage ("Folder picture set");
                    }
                    else if (onMessage) onMessage (error);
                }
            }
            if (onDone) onDone();
        });
    }

    // Owner: "ask when importing." Which folder the new files go in -- a new
    // one (the default for several files), none (the default for one), or
    // an existing folder -- with an optional picture. `then` runs afterwards
    // either way (the metadata prompt).
    void promptImportFolder (const juce::StringArray& assetIds, ezlibrary::Library* lib, std::function<void()> then)
    {
        juce::StringArray names;
        for (auto& id : assetIds) if (auto e = lib->findById (id)) names.add (e->name);
        const auto existing = sortedFolders (*lib);
        const juce::String suggested = ezlibrary::Library::suggestFolderName (names);

        const juce::String message = assetIds.size() == 1
            ? "Put \"" + names[0] + "\" in a folder?"
            : "Put the " + juce::String (assetIds.size()) + " imported files in a folder? Linked stems stay together, and you can give the folder a picture.";
        auto* aw = new juce::AlertWindow ("Folder", message, juce::MessageBoxIconType::NoIcon);

        juce::StringArray choices { "New folder", "No folder" };
        for (auto& f : existing) choices.add ("Add to: " + f.name);
        aw->addComboBox ("where", choices, "Where");
        aw->addTextEditor ("name", suggested, "New folder name");

        auto* combo = aw->getComboBoxComponent ("where");
        auto* nameEditor = aw->getTextEditor ("name");
        combo->setSelectedItemIndex (assetIds.size() >= 2 ? 0 : 1, juce::dontSendNotification);
        nameEditor->setEnabled (combo->getSelectedItemIndex() == 0);
        combo->onChange = [combo, nameEditor] { nameEditor->setEnabled (combo->getSelectedItemIndex() == 0); };

        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("OK + Picture...", 2);
        aw->addButton ("Skip", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, assetIds, lib, existing, suggested, then] (int result)
            {
                const int choice = aw->getComboBoxComponent ("where")->getSelectedItemIndex();
                const juce::String typed = aw->getTextEditorContents ("name").trim();
                delete aw;

                juce::String folderId;
                if (result != 0)
                {
                    if (choice == 0)
                    {
                        folderId = lib->createFolder (typed.isEmpty() ? suggested : typed, assetIds).id;
                    }
                    else if (choice >= 2 && choice - 2 < (int) existing.size()
                             && lib->findFolder (existing[(size_t) (choice - 2)].id).has_value())
                    {
                        folderId = existing[(size_t) (choice - 2)].id;
                        for (auto& id : assetIds) lib->setEntryFolder (id, folderId);
                    }

                    if (folderId.isNotEmpty())
                    {
                        lib->save();
                        refreshCards();
                        if (onMessage) onMessage ("Added to folder \"" + lib->findFolder (folderId)->name + "\"");
                    }
                }

                if (result == 2 && folderId.isNotEmpty()) choosePictureForFolder (folderId, then);
                else if (then) then();
            }), false);
    }

    // Milestone 16-T2: picks one or more audio files and imports each into
    // the first Library root (LibraryManager always has at least one --
    // SessionComponent's constructor adds a default Documents/EzPlay/Library
    // root). Async, per JUCE's own FileChooser convention (never blocks the
    // message thread waiting on a native OS dialog).
    void chooseFilesToImport()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import audio into library...",
                                                         juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        // named to avoid shadowing juce::Component's own internal `flags`
        // member (this local previously did, MSVC C4458) -- a harmless but
        // avoidable warning, cleaned up as part of the dev-experience pass.
        constexpr auto chooserFlags = juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems;

        chooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
        {
            importFiles (fc.getResults());
        });
    }

    // OS files dropped directly on the Library -- dropping HERE already is
    // the "add to library" intent, so no confirmation ask, straight in.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return anySupportedAudioPath (files); }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        juce::Array<juce::File> asFiles;
        for (const auto& p : files) if (isSupportedAudioPath (p)) asFiles.add (juce::File (p));
        importFiles (asFiles);
    }

public:
    // The single import entry point -- used by the + IMPORT chooser above,
    // OS-file drops on the Library, and SessionComponent's "add to Library
    // too?" offers after a drop elsewhere. Body is the chooser's old
    // callback, unchanged: import + stem-set grouping + metadata prompting.
    void importFiles (const juce::Array<juce::File>& files)
    {
        if (files.isEmpty() || libraryManager.libraries().empty()) return;

        // Owner #4: "app freezes when trying to upload multiple files into
        // library." The freeze was this loop decoding/hashing every file on
        // the message thread. The expensive half (analyzeImportFile -- hash,
        // decode, BPM) now runs on this tab's own single worker thread with
        // a per-file progress toast; the Library half
        // (registerAnalyzedImport -- dedup, upsert, save) stays on the
        // message thread in finishBackgroundImport(), so the Library is
        // never touched off-thread. One import at a time -- the guard below
        // keeps a second drop/chooser from interleaving with a running one.
        if (importRunning.exchange (true))
        {
            if (onMessage) onMessage ("An import is already running -- wait for it to finish");
            return;
        }

        juce::Component::SafePointer<MySamplesTab> safe (this);
        auto* fm = &formatManager;   // SessionComponent's app-lifetime AudioFormatManager -- outlives any job
        importPool.addJob ([safe, files, fm]
        {
            std::vector<eximport::AnalyzedImport> analyzed;
            analyzed.reserve ((size_t) files.size());
            int idx = 0;
            for (auto& file : files)
            {
                ++idx;
                const juce::String progress = "Importing " + juce::String (idx) + "/"
                                                + juce::String (files.size()) + ": " + file.getFileName();
                juce::MessageManager::callAsync ([safe, progress]
                {
                    if (safe != nullptr && safe->onMessage) safe->onMessage (progress);
                });
                analyzed.push_back (eximport::analyzeImportFile (file, *fm));
            }
            juce::MessageManager::callAsync ([safe, analyzed]
            {
                if (safe != nullptr) safe->finishBackgroundImport (analyzed);
            });
        });
    }

private:
    // Message thread. The Library-touching half of importFiles() above, plus
    // everything that always followed the import loop (stem-set grouping,
    // card refresh, summary toast, metadata prompting) -- moved, not changed.
    void finishBackgroundImport (const std::vector<eximport::AnalyzedImport>& analyzedFiles)
    {
        importRunning.store (false);
        if (libraryManager.libraries().empty()) return;

        {
            auto& targetLibrary = *libraryManager.libraries().front();
            int imported = 0, duplicates = 0, failed = 0;
            juce::String firstName;
            juce::StringArray importedAssetIds;
            juce::StringArray folderCandidateIds;   // new files plus ones already in the library -- both can go in the folder
            for (auto& analyzed : analyzedFiles)
            {
                auto result = eximport::registerAnalyzedImport (targetLibrary, analyzed);
                if (result.success && result.entry.isValid()) folderCandidateIds.addIfNotAlreadyThere (result.entry.assetId);
                if (! result.success) ++failed;
                else if (result.wasDuplicate) ++duplicates;
                else
                {
                    ++imported;
                    if (firstName.isEmpty()) firstName = result.entry.name;
                    importedAssetIds.add (result.entry.assetId);
                }
            }

            // Grouping a multi-file import used to happen silently here; the
            // user now chooses the folder (promptImportFolder() below).

            refreshCards();
            if (onMessage)
            {
                // §6: "Imported <name>" for one, "Imported N samples" for
                // several -- extended (not replaced) with duplicate/failed
                // counts when relevant, an existing detail worth keeping.
                juce::String msg = imported == 1 ? ("Imported " + firstName) : (juce::String (imported) + " imported");
                if (duplicates > 0) msg += ", " + juce::String (duplicates) + " already in library";
                if (failed > 0)     msg += ", " + juce::String (failed) + " failed";
                onMessage (msg);
            }

            // Owner: "ask when importing" -- which folder first, then the
            // metadata prompt (owner request: "when uploading files to the
            // library give me the option to insert the metadata"). One file:
            // straight into the metadata editor (Cancel skips it). Several:
            // ask once, then walk the editor through each imported file.
            auto* libPtr = &targetLibrary;
            auto askMetadata = [this, importedAssetIds, libPtr]
            {
                if (importedAssetIds.size() == 1)
                {
                    promptEditAssetMetadata (importedAssetIds[0], libPtr);
                }
                else if (importedAssetIds.size() >= 2)
                {
                    auto* ask = new juce::AlertWindow ("Add Metadata",
                                                        "Add metadata to the " + juce::String (importedAssetIds.size())
                                                          + " imported samples now?",
                                                        juce::MessageBoxIconType::NoIcon);
                    ask->addButton ("Add Metadata", 1, juce::KeyPress (juce::KeyPress::returnKey));
                    ask->addButton ("Not Now", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                    ask->enterModalState (true, juce::ModalCallbackFunction::create (
                        [this, ask, importedAssetIds, libPtr] (int result)
                        {
                            delete ask;
                            if (result == 1) editMetadataSequentially (importedAssetIds, 0, libPtr);
                        }), false);
                }
            };

            if (folderCandidateIds.isEmpty()) askMetadata();
            else promptImportFolder (folderCandidateIds, libPtr, askMetadata);
        }
    }

private:

    ezlibrary::LibraryManager& libraryManager;
    juce::AudioFormatManager& formatManager;
    std::function<void (juce::String)> onMessage;
    std::function<void (std::shared_ptr<const std::vector<float>>, std::shared_ptr<const std::vector<float>>, double)> onStartPreview;
    std::function<void()> onStopPreview;
    std::function<bool()> isPreviewActive;

    // Owner #4 background import -- ONE worker thread (imports are
    // sequential by design; the importRunning guard enforces it at the API
    // too). Pool member (not Thread::launch) so this tab's destructor waits
    // for a running job instead of leaving a detached thread using a dead
    // SafePointer's target mid-teardown.
    juce::ThreadPool importPool { 1 };
    std::atomic<bool> importRunning { false };

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Label titleLabel, emptyStateLabel;
    juce::TextButton importButton, emptyImportButton, gridViewButton, listViewButton;
    juce::OwnedArray<juce::TextButton> filterButtons;
    SearchBar searchBox;
    juce::Viewport viewport;
    juce::Component gridHolder;
    juce::OwnedArray<LibrarySampleCard> cards;
    juce::OwnedArray<LibraryFolderCard> folderCards;
    std::vector<juce::Component*> tiles;   // folder cards then sample cards, in display order
    std::map<juce::String, juce::Image> folderImages;
    juce::StringArray selectedIds;
    bool selectMode { false };
    juce::String openFolderId;             // empty = the top level
    juce::TextButton newFolderButton, backButton, folderOptionsButton,
                     selectionNewFolderButton, selectionMoveButton, selectionDoneButton;
    juce::Label barLabel;
    std::unique_ptr<juce::FileChooser> pictureChooser;
    std::map<juce::String, LoadedSample> sampleCache;
    juce::String currentlyAuditioningAssetId;
    juce::Rectangle<int> browseArea;
    CategoryFilter categoryFilter { CategoryFilter::all };
    SortField sortField { SortField::name };
    juce::ComboBox sortBox;
    bool listViewActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MySamplesTab)
};


//==============================================================================
// SPEC_M6_EDIT_WINDOW.md §B step 3: header + controls-row redesign.
// closeButton replaces the old separate "Done" button -- one adaptive close
// affordance instead of two: onClose (docked case, collapses the dock) if
// provided, else falls back to the pre-existing modal-exit behaviour (so
// PERFORM's own double-click-to-modal path, which doesn't pass onClose,
// keeps working unchanged). previewButton now reflects REAL preview state
// (isPreviewActive, polled on a 15Hz Timer) rather than a locally-guessed
// bool -- same "read real engine state, never fake it" convention the app
// already uses for the stem-editor transport (stemEditorDeckIsPlaying()).
// Honest gap (SPEC_M6_EDIT_WINDOW.md §B's "loop toggle"): there is no
// existing per-layer OR per-deck "loop on/off" concept in Deck.h to bind a
// toggle to -- loop-mode decks always loop by definition, and stem mode's
// only related concept is the deck-level (not layer-level) stemEndBehavior.
// Faking a toggle with nothing real behind it would violate "reuse existing
// logic, don't invent" and "no engine touch" -- omitted, flagged here rather
// than silently dropped.
//==============================================================================
//  Owner: "can u make the set tempo scrolable to make it touch convinient."
//  A numeric readout you change by DRAGGING it (vertically, like every DAW's
//  tempo field) or by scrolling over it -- instead of only through a modal
//  "Set..." dialog with a keyboard. Drag is the touch-friendly half: it needs
//  no keyboard and no precise target, and a slow drag gives fine control
//  (1 BPM per 4px) while a fast one covers ground.
//
//  Double-click still opens the exact-entry dialog (onEditRequested), because
//  typing "128" is faster than dragging to it when you already know the value.
//  onDragStart fires once per gesture so the owner can push a single undo
//  snapshot, matching WaveformView's own onEditStarting contract.
//==============================================================================
class ScrubValueLabel : public juce::Component, public juce::SettableTooltipClient
{
public:
    ScrubValueLabel() = default;

    std::function<void (double)> onValueChanged;   // new value, already clamped
    std::function<void()>        onDragStart;      // once per drag gesture (undo snapshot)
    std::function<void()>        onEditRequested;  // double-click -> exact entry

    void setRange (double minV, double maxV, double stepV) { minValue = minV; maxValue = maxV; step = stepV; }
    void setValue (double v) { value = juce::jlimit (minValue, maxValue, v); repaint(); }
    double getValue() const { return value; }
    void setSuffix (const juce::String& s) { suffix = s; repaint(); }
    void setAccentColour (juce::Colour c)  { accent = c; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (juce::Colour (0xff151527u));
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (dragging ? accent : juce::Colour (0xff2b2b4du));
        g.drawRoundedRectangle (b, 6.0f, dragging ? 1.6f : 1.0f);

        g.setColour (juce::Colour (0xfff2f0ffu));
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::bold)));
        g.drawText (juce::String (value, 1) + suffix, getLocalBounds().reduced (6, 0),
                    juce::Justification::centred, false);

        // tiny up/down chevrons -- the affordance that says "this is draggable"
        g.setColour (juce::Colour (0xff6f7099u));
        const float cx = b.getRight() - 9.0f;
        juce::Path up, down;
        up.addTriangle   (cx - 3.5f, b.getY() + 8.0f,  cx + 3.5f, b.getY() + 8.0f,  cx, b.getY() + 3.5f);
        down.addTriangle (cx - 3.5f, b.getBottom() - 8.0f, cx + 3.5f, b.getBottom() - 8.0f, cx, b.getBottom() - 3.5f);
        g.fillPath (up);
        g.fillPath (down);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging      = true;
        startedDrag   = false;
        dragStartY    = e.position.y;
        dragStartValue = value;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        const float dy = dragStartY - e.position.y;          // up = increase
        if (! startedDrag)
        {
            if (std::abs (dy) < 3.0f) return;                // click-vs-drag guard
            startedDrag = true;
            if (onDragStart) onDragStart();
        }
        // 4px per step; Shift = fine (1px per step)
        const double perPixel = e.mods.isShiftDown() ? step : step * 0.25;
        commit (dragStartValue + (double) dy * perPixel);
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; startedDrag = false; repaint(); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (wheel.deltaY == 0.0f) return;
        if (onDragStart) onDragStart();
        commit (value + (wheel.deltaY > 0.0f ? 1.0 : -1.0) * (e.mods.isShiftDown() ? step * 0.1 : step));
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { if (onEditRequested) onEditRequested(); }

private:
    void commit (double v)
    {
        const double clamped = juce::jlimit (minValue, maxValue, v);
        if (juce::approximatelyEqual (clamped, value)) return;
        value = clamped;
        repaint();
        if (onValueChanged) onValueChanged (value);
    }

    double value { 120.0 }, minValue { 1.0 }, maxValue { 999.0 }, step { 1.0 };
    double dragStartValue { 120.0 };
    float  dragStartY { 0.0f };
    bool   dragging { false }, startedDrag { false };
    juce::String suffix;
    juce::Colour accent { juce::Colour (0xff7c5cffu) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScrubValueLabel)
};

class ClipEditorContent : public juce::Component,
                          private juce::Timer,
                          private juce::ScrollBar::Listener
{
public:
    ClipEditorContent (const juce::String& clipName, const juce::String& stats,
                        ezdeck::Layer& layerRef, double& taggedBpmRef, double layerSampleRate,
                        juce::Colour accentColour,
                        std::function<void (int, bool, bool)> onPreviewFn, std::function<void()> onStopPreviewFn,
                        std::function<bool()> isPreviewActiveFn,
                        std::function<void()> onCloseFn = std::function<void()>(),
                        std::function<void()> onLayerEditedFn = std::function<void()>(),
                        std::function<void (int)> onScrubFn = std::function<void (int)>(),
                        // Owner: "I should see the playhead moving in the editor."
                        // Returns the clip's current ABSOLUTE sample position while
                        // it is actually sounding, or -1 when it isn't. Supplied by
                        // SessionComponent, which is the only thing that knows both
                        // the deck's playhead and the editor preview's position.
                        std::function<int()> livePlayheadFn = std::function<int()>())
        : waveform (layerRef), layer (layerRef), taggedBpm (taggedBpmRef), sampleRate (layerSampleRate),
          onPreview (std::move (onPreviewFn)), onStopPreview (std::move (onStopPreviewFn)),
          isPreviewActive (std::move (isPreviewActiveFn)), livePlayhead (std::move (livePlayheadFn)),
          onClose (std::move (onCloseFn)),
          onLayerEdited (std::move (onLayerEditedFn)), onScrub (std::move (onScrubFn))
    {
        waveform.onEdited = [this] { if (onLayerEdited) onLayerEdited(); };
        waveform.onEditStarting = [this] { pushUndoSnapshot(); };
        // roadmap "scrubbing": forwarded straight through to the owner --
        // only meaningful while a preview is already active (see
        // scrubPreviewTo()'s own guard), so no isPreviewActive check needed
        // here specifically.
        waveform.onScrub = [this] (int sampleIndex) { if (onScrub) onScrub (sampleIndex); };
        nameLabel.setText ("EDITING: " + clipName, juce::dontSendNotification);
        nameLabel.setFont (performfonts::headingFont (16.0f));   // editor heading -- Space Grotesk, like every other heading
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));   // == performlive::kTextBright
        addAndMakeVisible (nameLabel);

        statsLabel.setText (stats, juce::dontSendNotification);
        statsLabel.setJustificationType (juce::Justification::topLeft);
        statsLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6f7099u));   // == performlive::kTextFaint
        statsLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (statsLabel);

        // Owner: "can u make the set tempo scrolable to make it touch
        // convinient" -- the tagged tempo is now a drag/scroll field (the
        // "Set..." dialog is still there on double-click for exact entry).
        // Every change re-pushes the grid, so the DAW grid follows the tempo
        // live as you drag it.
        bpmField.setRange (1.0, 999.0, 1.0);
        bpmField.setSuffix (" BPM");
        bpmField.setAccentColour (accentColour);
        bpmField.setValue (taggedBpm);
        bpmField.setTooltip ("The clip's tempo, detected on load. Drag up/down or scroll to correct it; double-click to type it. Shift = fine.");
        bpmField.onDragStart = [this] { pushUndoSnapshot(); };
        bpmField.onValueChanged = [this] (double v)
        {
            taggedBpm = juce::jlimit (1.0, 999.0, v);
            waveform.setBarGrid (taggedBpm, sampleRate);
            refreshBpmLabel();
        };
        bpmField.onEditRequested = [this] { promptSetBpm(); };
        addAndMakeVisible (bpmField);

        bpmLabel.setJustificationType (juce::Justification::centredLeft);
        bpmLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6f7099u));   // == performlive::kTextFaint
        bpmLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
        addAndMakeVisible (bpmLabel);
        refreshBpmLabel();

        // SPEC_M6_EDIT_WINDOW.md §B: the waveform's identity colour (matches
        // the lane it came from) and its bar-grid context -- both presentation
        // only, wired once here and re-pushed wherever taggedBpm changes below.
        waveform.setAccentColour (accentColour);
        waveform.setBarGrid (taggedBpm, sampleRate);
        waveform.setGridDivisionsPerBar (16);   // owner: "grid should be on 1/16 by default"
        addAndMakeVisible (waveform);

        previewButton.onClick = [this]
        {
            const bool active = isPreviewActive && isPreviewActive();
            if (active) { if (onStopPreview) onStopPreview(); }
            else        { if (onPreview) onPreview (waveform.getPreviewPosition(), loopPreviewButton.getToggleState(), soloPreviewButton.getToggleState()); }
        };
        addAndMakeVisible (previewButton);
        refreshPreviewButtonState();
        // 30Hz: the owner's moving playhead needs to look smooth, and this
        // timer now drives it as well as the preview-button state. Still
        // UI-only -- it reads a position, it never drives audio.
        startTimerHz (30);

        // roadmap "Loop Preview"/"Solo Preview": plain toggles read at the
        // moment Preview is pressed (see previewButton.onClick above) --
        // doesn't affect an ALREADY-playing preview, matching how e.g.
        // LOCK's own toggle-state reads are scoped elsewhere in this app
        // (the next action picks up the new state, not a live-in-flight one).
        loopPreviewButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xba")));   // curved loop arrow
        loopPreviewButton.setClickingTogglesState (true);
        loopPreviewButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527u));   // == performlive::kCard
        loopPreviewButton.setColour (juce::TextButton::buttonOnColourId, accentColour);
        addAndMakeVisible (loopPreviewButton);

        soloPreviewButton.setButtonText ("S");
        soloPreviewButton.setClickingTogglesState (true);
        soloPreviewButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527u));   // == performlive::kCard
        soloPreviewButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffc933u));   // == performlive::kQueued -- established amber "temporary state" colour
        soloPreviewButton.setTooltip ("Solo Preview: mute the other layers on this deck while previewing");
        addAndMakeVisible (soloPreviewButton);

        // roadmap "Undo": disabled until the first edit lands (refreshed by
        // pushUndoSnapshot()/undoLastEdit()).
        undoButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xb6")));   // undo arrow
        undoButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff151527u));   // == performlive::kCard
        undoButton.onClick = [this] { undoLastEdit(); };

        // Editor V2 top-right session cluster. The brief is explicit that
        // this corner carries session actions and never Play -- Play moves
        // to the single transport cluster under the waveform.
        //
        // Save and Apply from the brief are deliberately NOT both here.
        // Every edit in this editor is already live on the layer the moment
        // it happens, so an "Apply" would be a button that does nothing --
        // exactly the mystery control the same brief forbids. Done closes
        // the editor, which is the real end of the task.
        redoButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xb7")));
        redoButton.setTooltip ("Redo");
        redoButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff151527u));
        redoButton.onClick = [this] { redoLastEdit(); };
        redoButton.setEnabled (false);
        addAndMakeVisible (redoButton);

        doneButton.setButtonText ("Done");
        doneButton.setTooltip ("Close the editor");
        doneButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff151527u));
        doneButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff2ee86au));   // green = completion, per the brief
        doneButton.onClick = [this] { if (onClose) onClose(); };

        // Numbered because these panels genuinely ARE a sequence here: set the
        // clip's tempo, then its level, then how you are looking at it. The
        // numbering is the brief's own device and it encodes that order rather
        // than decorating the headings.
        auto section = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      9.5f, juce::Font::bold))
                           .withExtraKerningFactor (0.16f));
            l.setColour (juce::Label::textColourId, juce::Colour (0xff6f7099u));   // kTextFaint
            l.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (l);
        };
        section (tempoSectionLabel, "01  TEMPO & LENGTH");
        section (gainSectionLabel,  "02  GAIN");
        section (viewSectionLabel,  "03  VIEW");
        addAndMakeVisible (doneButton);
        addAndMakeVisible (undoButton);
        refreshUndoButtonState();

        // Owner: "there are 2 close signs in the edit window... remove the
        // Hide X" -- the editor dock's own header already has the one close
        // (editorCloseButton -> closeEditorDock()); the in-editor Hide
        // button is gone from both editors.

        for (auto* b : { &bpmHalveButton, &bpmDoubleButton, &setBpmButton, &fitFourBarsButton,
                          &snapToGridButton, &resetMarkersButton })
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff151527u));   // == performlive::kCard

        bpmHalveButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\xb7")) + "2");
        bpmHalveButton.onClick = [this] { pushUndoSnapshot(); taggedBpm = juce::jmax (1.0, taggedBpm * 0.5); refreshBpmLabel(); waveform.setBarGrid (taggedBpm, sampleRate); };
        addAndMakeVisible (bpmHalveButton);

        bpmDoubleButton.setButtonText ("x2");
        bpmDoubleButton.onClick = [this] { pushUndoSnapshot(); taggedBpm = juce::jmin (999.0, taggedBpm * 2.0); refreshBpmLabel(); waveform.setBarGrid (taggedBpm, sampleRate); };
        addAndMakeVisible (bpmDoubleButton);

        setBpmButton.setButtonText ("Set...");
        setBpmButton.onClick = [this] { promptSetBpm(); };
        addAndMakeVisible (setBpmButton);

        fitFourBarsButton.setButtonText ("Fit 4 Bars");
        fitFourBarsButton.onClick = [this]
        {
            // manual counterpart to loadLayer's automatic fit (M3-T3) -- uses
            // the CURRENT tagged tempo, not a fresh re-detection, matching
            // the reference's own tool relationships (Fit 4 bars consumes
            // whatever tempo is currently tagged, including one the user
            // just adjusted via BPM /2, x2, or Set BPM)
            pushUndoSnapshot();
            const double duration = (double) layer.numFrames() / sampleRate;
            auto fit = ezdsp::fitFourBars (duration, taggedBpm);
            // SPEC_WARP_BUG_INVESTIGATION.md fix: `duration` above was read at
            // the START of this handler -- if a re-warp's swap lands on this
            // SAME layer before this line runs, numFrames() may already be
            // smaller than what `duration` assumed. setRegionLengthClamped()
            // (Deck.h) re-checks against the CURRENT numFrames() right here,
            // so the stored value can never describe a buffer that no longer
            // exists, regardless of that race's timing. See its own comment
            // for the full root-cause trace.
            layer.setRegionLengthClamped ((int) std::llround (fit.end * sampleRate));
            layer.trimmed      = true;
            taggedBpm          = fit.bpm;   // fitFourBars may octave-correct; reflect that in the tag
            refreshBpmLabel();
            waveform.setBarGrid (taggedBpm, sampleRate);
            waveform.repaint();
            if (onLayerEdited) onLayerEdited();
        };
        addAndMakeVisible (fitFourBarsButton);

        // Owner: "I should be able to zoom into the waveform to see the
        // beginning and ends of the track just in case I want to change the
        // start and end markers." Ctrl+wheel zoom already existed but is
        // undiscoverable and unusable on a touchscreen -- these are real
        // 44px-tall buttons. |< and >| jump the view to the start/end marker
        // at the current zoom, which is what makes fine marker work possible.
        for (auto* b : { &zoomOutButton, &zoomInButton, &zoomFitButton, &gotoStartButton, &gotoEndButton })
        {
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff151527u));
            addAndMakeVisible (b);
        }
        zoomOutButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")));   // minus
        zoomOutButton.onClick = [this] { waveform.zoomBy (1.0f / 1.6f); refreshZoomLabel(); };
        zoomInButton.setButtonText ("+");
        zoomInButton.onClick  = [this] { waveform.zoomBy (1.6f); refreshZoomLabel(); };
        zoomFitButton.setButtonText ("FIT");
        zoomFitButton.onClick = [this] { waveform.zoomToFit(); refreshZoomLabel(); };
        gotoStartButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x9d\xae")) + "|");
        gotoStartButton.setTooltip ("Jump the view to the start marker");
        gotoStartButton.onClick = [this] { waveform.centreOnSample (layer.regionStartClamped()); };
        gotoEndButton.setButtonText ("|" + juce::String (juce::CharPointer_UTF8 ("\xe2\x9d\xaf")));
        gotoEndButton.setTooltip ("Jump the view to the end marker");
        gotoEndButton.onClick = [this] { waveform.centreOnSample (layer.regionStartClamped() + layer.loopLength()); };

        zoomLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::plain)));
        zoomLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6f7099u));
        zoomLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (zoomLabel);
        refreshZoomLabel();

        // Owner: "when I zoom in in the edit window, I need a scroll bar to
        // scroll left and right." Wheel-panning existed but nothing showed
        // WHERE you were in the clip, and there was nothing to drag on a
        // touchscreen. A real scrollbar both scrolls and shows position; it
        // disables itself at 1x, where there is nothing to scroll.
        waveScrollBar.setAutoHide (false);
        waveScrollBar.addListener (this);
        addAndMakeVisible (waveScrollBar);
        waveform.onViewChanged = [this] { syncScrollBarToView(); };
        syncScrollBarToView();

        // Owner: "I should be able to adjust the audio clip to put it on the
        // grid well if I want to." The grid is anchored to the START MARKER
        // (see WaveformView::paint), so nudging that marker is what slides
        // the audio against the grid. These are fine, repeatable steps --
        // dragging is too coarse when you're a few milliseconds out, and on
        // a touchscreen it's fiddly at any zoom.
        for (auto* b : { &nudgeLeftButton, &nudgeRightButton, &alignToGridButton })
        {
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff151527u));
            addAndMakeVisible (b);
        }
        nudgeLeftButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x80")));
        nudgeLeftButton.setTooltip ("Nudge the clip earlier against the grid (Shift = 10x finer)");
        nudgeLeftButton.onClick  = [this] { nudgeStart (-1); };
        nudgeRightButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6")));
        nudgeRightButton.setTooltip ("Nudge the clip later against the grid (Shift = 10x finer)");
        nudgeRightButton.onClick = [this] { nudgeStart (1); };
        alignToGridButton.setButtonText ("ALIGN");
        alignToGridButton.setTooltip ("Snap the start marker to the nearest grid line");
        alignToGridButton.onClick = [this]
        {
            const double step = waveform.gridStepSamples();
            if (step < 1.0) return;
            pushUndoSnapshot();
            const int start   = layer.regionStartClamped();
            const int snapped = (int) (juce::roundToInt ((double) start / step) * step);
            const int oldEnd  = start + layer.loopLength();
            layer.setRegionStartClamped (snapped);
            layer.setRegionLengthClamped (juce::jmax (1, oldEnd - layer.regionStartClamped()));
            layer.trimmed = true;
            waveform.repaint();
            if (onLayerEdited) onLayerEdited();
        };

        // Owner: "the window should be a grid 1/4 1/2 1/16 1/8 1/32 etc...
        // like a standard DAW audio editing window." The selector drives BOTH
        // what's drawn and what Snap Grid snaps to -- the grid you see is the
        // grid you snap to, which is the whole point of a DAW grid.
        gridLabel.setText ("GRID", juce::dontSendNotification);
        gridLabel.setFont (performfonts::headingFont (10.0f).withExtraKerningFactor (0.06f));
        gridLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6f7099u));
        gridLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (gridLabel);

        // ids are 1-based menu ids; the VALUE is divisions-per-bar (4/4)
        gridBox.addItem ("Bar",  1);   // 1  division  per bar
        gridBox.addItem ("1/2",  2);   // 2
        gridBox.addItem ("1/4",  3);   // 4
        gridBox.addItem ("1/8",  4);   // 8
        gridBox.addItem ("1/16", 5);   // 16
        gridBox.addItem ("1/32", 6);   // 32
        gridBox.addItem ("Off",  7);   // 0
        gridBox.setSelectedId (5, juce::dontSendNotification);   // owner: 1/16 default
        gridBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff151527u));
        gridBox.setColour (juce::ComboBox::textColourId,       juce::Colour (0xfff2f0ffu));
        gridBox.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff2b2b4du));
        gridBox.setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xffa3a6ccu));
        gridBox.onChange = [this]
        {
            waveform.setGridDivisionsPerBar (divisionsForGridId (gridBox.getSelectedId()));
            refreshSnapButtonText();
        };
        addAndMakeVisible (gridBox);

        snapToGridButton.setButtonText ("Snap 1/16");
        snapToGridButton.onClick = [this]
        {
            // Snaps the region END to the nearest line of the CURRENTLY
            // SELECTED grid (was always a beat, regardless of the grid) --
            // there is still no region-START concept to snap (same limitation
            // WaveformView's region-end handle already documents).
            const double step = waveform.gridStepSamples();
            if (step < 1.0) return;   // grid off, or no tempo context -- nothing to snap to
            pushUndoSnapshot();
            const int gridSamples = juce::jmax (1, (int) std::llround (step));
            const int current     = layer.loopLength();
            const int snapped     = juce::jlimit (1, layer.numFrames(),
                                                   (int) (juce::roundToInt ((double) current / (double) gridSamples) * gridSamples));
            layer.regionLength = snapped;
            layer.trimmed      = true;
            waveform.repaint();
            if (onLayerEdited) onLayerEdited();
        };
        addAndMakeVisible (snapToGridButton);

        resetMarkersButton.setButtonText ("Reset");
        resetMarkersButton.onClick = [this]
        {
            pushUndoSnapshot();
            // "Region back to the whole file" (PRODUCT_REQUIREMENTS.md §7).
            // Deliberately scoped to region/trimmed only -- fades are a
            // distinct clip field in the reference's own data model and are
            // untouched by this specific tool.
            layer.regionLength = 0;
            layer.trimmed      = false;
            waveform.repaint();
            if (onLayerEdited) onLayerEdited();
        };
        addAndMakeVisible (resetMarkersButton);

        // SPEC_M6_EDIT_WINDOW.md §B: "Gain -- a horizontal slider + dB
        // readout (reuse existing gain logic)". Layer::gain (Deck.h) already
        // multiplies the audio thread's output (render()/renderPerTab()) --
        // it simply never had a UI control before this. Same category of
        // field as regionLength/fadeInSamples (plain float, not atomic,
        // written from the message thread) -- same pre-existing data-race
        // caveat this class's header comment already documents for those,
        // not a new risk this control introduces.
        gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        gainSlider.setRange (0.0, 2.0, 0.001);
        gainSlider.setValue (layer.gain, juce::dontSendNotification);
        gainSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        gainSlider.setColour (juce::Slider::trackColourId, juce::Colour (0xff2b2b4du));   // == performlive::kBorder
        gainSlider.setColour (juce::Slider::thumbColourId, accentColour);
        gainSlider.onValueChange = [this] { layer.gain = (float) gainSlider.getValue(); refreshGainLabel(); waveform.repaint(); };   // repaint: gain overlay
        gainSlider.onDragStart = [this] { pushUndoSnapshot(); };   // roadmap "Undo": once per drag gesture, not per onValueChange tick
        addAndMakeVisible (gainSlider);

        gainDbLabel.setJustificationType (juce::Justification::centredRight);
        gainDbLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));   // == performlive::kTextDim
        addAndMakeVisible (gainDbLabel);
        refreshGainLabel();

        // roadmap "Normalize": sets Layer::gain so the loudest sample within
        // the CURRENTLY TRIMMED region (loopLength(), not the whole raw
        // buffer -- normalizing against audio the deck won't even play would
        // be surprising) hits exactly 0dBFS. Silent/near-silent clips are
        // left alone rather than dividing by ~zero into an extreme gain.
        // Owner #2: "select Normalize to normalize, deselect to undo it" --
        // a TOGGLE now: on remembers the pre-normalize gain and applies the
        // 0dBFS-peak gain; off restores exactly the remembered gain.
        normalizeButton.setButtonText ("Normalize");
        normalizeButton.setClickingTogglesState (true);
        normalizeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527u));   // == performlive::kCard
        normalizeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff7c5cffu));   // == performlive::kIndigo
        normalizeButton.onClick = [this]
        {
            if (normalizeButton.getToggleState())
            {
                const int n = juce::jmin (layer.loopLength(), layer.numFrames());
                float peak = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    peak = juce::jmax (peak, std::abs (layer.left[(size_t) i]));
                    if (! layer.right.empty()) peak = juce::jmax (peak, std::abs (layer.right[(size_t) i]));
                }
                if (peak > 0.001f)
                {
                    pushUndoSnapshot();
                    preNormalizeGain = layer.gain;
                    layer.gain = juce::jlimit (0.0f, 2.0f, 1.0f / peak);
                }
                else
                {
                    normalizeButton.setToggleState (false, juce::dontSendNotification);   // nothing to normalise
                }
            }
            else
            {
                layer.gain = preNormalizeGain;
            }
            gainSlider.setValue (layer.gain, juce::dontSendNotification);
            refreshGainLabel();
            waveform.repaint();
        };
        addAndMakeVisible (normalizeButton);

        // SPEC_PERFORM_V2 GROUP D / GROUP I: the "AI-ready placeholder" row
        // (Detect Loop / Generate Pads / Detect Sections / Generate
        // Arrangement) is removed entirely -- confirmed inert (every button
        // was permanently setEnabled(false), no handler ever wired to any of
        // them) before deleting, so nothing behavioral is lost, only the
        // dead chrome. See resized() for where the freed space went.

        accent = accentColour;   // SPEC_PERFORM_V2 GROUP D: identity stripe, see paint()

        setSize (560, 520);
    }

    ~ClipEditorContent() override
    {
        stopTimer();
        // guarantees a preview started from this editor doesn't keep playing
        // once the editor is gone, regardless of how it was closed (Hide,
        // Escape, or the native close button)
        if (onStopPreview) onStopPreview();
    }

    void paint (juce::Graphics& g) override
    {
        // SPEC_PERFORM_V2 GROUP D "match the app's language, columnAccent
        // for the edited slot": this editor previously painted nothing of
        // its own at all (every pixel came from child components), which
        // read as unfinished next to the rest of the app's own card/dialog
        // language. A plain shell background plus a left-edge accent stripe
        // (the same accent the waveform/gain-slider already use) gives the
        // window a real, consistent identity tying it to the specific slot
        // being edited.
        // These two literals predated the neon palette pass and were missed by
        // it -- they were near-misses for the old shell/border values rather
        // than exact matches, so the search-and-replace never saw them. Now on
        // the real tokens.
        g.setColour (juce::Colour (0xff0c0c17u));   // == performlive::kShellBg
        g.fillAll();
        g.setColour (accent);
        g.fillRect (getLocalBounds().removeFromLeft (3));
        g.setColour (juce::Colour (0xff2b2b4du));   // == performlive::kBorder
        g.drawLine (0.0f, 52.0f, (float) getWidth(), 52.0f, 1.0f);

        // Editor V2 control panels. The brief groups the controls into named
        // sections rather than leaving them as an undifferentiated strip of
        // buttons; these are those sections' surfaces, drawn behind the
        // controls resized() has already placed on them.
        auto panel = [&] (juce::Rectangle<int> r)
        {
            if (r.isEmpty()) return;
            g.setColour (juce::Colour (0xff151527u));   // == performlive::kCard
            g.fillRoundedRectangle (r.toFloat(), 8.0f);
            g.setColour (juce::Colour (0xff2b2b4du));
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 8.0f, 1.0f);
        };
        panel (tempoPanelRect);
        panel (gainPanelRect);
        panel (viewPanelRect);
    }

    void resized() override
    {
        auto area = getLocalBounds();

        // header (52px outer / 44px interior, touch pass: spec's "44px min
        // hit area" applied to the header's own buttons too, matching the
        // stem-editor toolbar's own 52px convention): name left, preview
        // toggle + close right
        // Editor V2: the header carries SESSION actions only. Preview, loop
        // and solo used to live up here beside Undo, which put two competing
        // "start something" controls in opposite corners; they now sit in the
        // one transport cluster below the waveform.
        auto header = area.removeFromTop (52).reduced (12, 4);
        doneButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 34));
        header.removeFromRight (10);
        redoButton.setBounds (header.removeFromRight (36).withSizeKeepingCentre (36, 36));
        header.removeFromRight (4);
        undoButton.setBounds (header.removeFromRight (36).withSizeKeepingCentre (36, 36));
        header.removeFromRight (10);
        nameLabel.setBounds (header);

        // secondary row: existing tools the spec's ascii diagram doesn't
        // show a slot for, but which "do not remove features" requires
        // keeping -- stats, tagged-tempo readout, Snap Grid, Reset, all
        // de-emphasised below the primary row (not the spec's explicitly-
        // named 44px controls, so kept compact rather than forced to 44px)
        // Editor V2: the single transport cluster, bottom-left, directly
        // under the waveform it drives. Taken off the bottom BEFORE the tool
        // row so it keeps a full-height 40px row of its own rather than
        // sharing one with the zoom and grid controls.
        {
            auto transportRow = area.removeFromBottom (44).reduced (12, 4);
            previewButton.setBounds     (transportRow.removeFromLeft (44).withSizeKeepingCentre (44, 36));
            transportRow.removeFromLeft (6);
            loopPreviewButton.setBounds (transportRow.removeFromLeft (36).withSizeKeepingCentre (36, 36));
            transportRow.removeFromLeft (4);
            soloPreviewButton.setBounds (transportRow.removeFromLeft (36).withSizeKeepingCentre (36, 36));
        }

        // Editor V2: VIEW panel -- zoom, grid, snap, align. Its surface is
        // painted in paint(); this only measures it and puts the heading on.
        {
            auto viewPanel = area.removeFromBottom (54).reduced (10, 2);
            viewPanelRect = viewPanel;
            viewSectionLabel.setBounds (viewPanel.removeFromTop (14).reduced (10, 0));
            area.removeFromBottom (2);
        }

        auto secondaryRow = viewPanelRect.withTrimmedTop (14).reduced (4, 2);
        // Right-to-left so the grid cluster keeps a fixed size and the stats
        // text (left) absorbs whatever width is left over.
        resetMarkersButton.setBounds (secondaryRow.removeFromRight (54).reduced (2));
        snapToGridButton.setBounds (secondaryRow.removeFromRight (78).reduced (2));
        secondaryRow.removeFromRight (4);
        gridBox.setBounds (secondaryRow.removeFromRight (68).reduced (0, 2));   // owner's DAW grid selector
        gridLabel.setBounds (secondaryRow.removeFromRight (32));
        secondaryRow.removeFromRight (6);
        // owner's grid-alignment cluster: ◀ ALIGN ▶ (nudges the start marker,
        // which is the grid's origin -- see WaveformView::paint)
        nudgeRightButton.setBounds (secondaryRow.removeFromRight (28).reduced (1));
        alignToGridButton.setBounds (secondaryRow.removeFromRight (54).reduced (1));
        nudgeLeftButton.setBounds  (secondaryRow.removeFromRight (28).reduced (1));
        secondaryRow.removeFromRight (6);
        // owner's zoom cluster: |< − 000% + FIT >|
        gotoEndButton.setBounds   (secondaryRow.removeFromRight (32).reduced (1));
        zoomFitButton.setBounds   (secondaryRow.removeFromRight (38).reduced (1));
        zoomInButton.setBounds    (secondaryRow.removeFromRight (32).reduced (1));
        zoomLabel.setBounds       (secondaryRow.removeFromRight (42));
        zoomOutButton.setBounds   (secondaryRow.removeFromRight (32).reduced (1));
        gotoStartButton.setBounds (secondaryRow.removeFromRight (32).reduced (1));
        secondaryRow.removeFromRight (8);
        statsLabel.setBounds (secondaryRow);

        // SPEC_PERFORM_V2 GROUP D "tighten the controls row -- the gain/
        // ÷2/x2/Set cluster reads cramped": the old layout crammed gain +
        // normalize + 3 tempo buttons + fit-4-bars into ONE 48px row via
        // nested percentage splits, leaving each control only a sliver.
        // Split into two full 44px rows instead (space freed by removing
        // the AI-placeholder row above, plus a taller window) -- every
        // control gets a real, even width rather than a fraction of one.
        // Owner's scrollable tempo takes the first slot in this row -- it IS
        // the tempo control now, with ÷2 / x2 / Fit 4 Bars beside it. ("Set..."
        // moved onto the field's own double-click, so the row keeps 4 slots.)
        // Editor V2: GAIN panel.
        auto gainPanel = area.removeFromBottom (62).reduced (10, 2);
        gainPanelRect = gainPanel;
        gainSectionLabel.setBounds (gainPanel.removeFromTop (14).reduced (10, 0));

        auto gainRow = gainPanel.reduced (6, 2);
        auto gainArea = gainRow.removeFromLeft (juce::roundToInt ((float) gainRow.getWidth() * 0.75f));
        gainDbLabel.setBounds (gainArea.removeFromRight (56));
        gainSlider.setBounds (gainArea.reduced (4, 0));
        gainRow.removeFromLeft (10);
        normalizeButton.setBounds (gainRow.reduced (2));


        // Editor V2: TEMPO & LENGTH panel.
        auto tempoPanel = area.removeFromBottom (62).reduced (10, 2);
        tempoPanelRect = tempoPanel;
        tempoSectionLabel.setBounds (tempoPanel.removeFromTop (14).reduced (10, 0));

        auto tempoRow = tempoPanel.reduced (6, 2);
        const int tempoGap = 8;
        const int tempoW = (tempoRow.getWidth() - tempoGap * 3) / 4;
        {
            auto bpmSlot = tempoRow.removeFromLeft (tempoW);
            bpmLabel.setBounds (bpmSlot.removeFromTop (11));
            bpmField.setBounds (bpmSlot.reduced (0, 1));
        }
        tempoRow.removeFromLeft (tempoGap);
        bpmHalveButton.setBounds (tempoRow.removeFromLeft (tempoW));
        tempoRow.removeFromLeft (tempoGap);
        bpmDoubleButton.setBounds (tempoRow.removeFromLeft (tempoW));
        tempoRow.removeFromLeft (tempoGap);
        fitFourBarsButton.setBounds (tempoRow);
        setBpmButton.setBounds ({});   // superseded by bpmField's double-click; kept alive for its existing wiring

        area.removeFromBottom (6);

        area.removeFromBottom (4);
        // Owner's horizontal scrollbar sits directly under the waveform, so
        // its thumb reads as "this is the slice of the clip you're looking
        // at". 14px is a real touch target without stealing waveform height.
        waveScrollBar.setBounds (area.removeFromBottom (14).reduced (0, 2));
        waveform.setBounds (area);
        syncScrollBarToView();   // width changed -> thumb proportions changed
    }

private:
    void timerCallback() override
    {
        refreshPreviewButtonState();
        // Owner's moving playhead: -1 (or no provider) simply hides it, so a
        // stopped clip shows no live line at all.
        const int pos = livePlayhead ? livePlayhead() : -1;
        waveform.setLivePlayhead (pos, pos >= 0);
    }

    void refreshPreviewButtonState()
    {
        const bool active = isPreviewActive && isPreviewActive();
        previewButton.setButtonText (juce::String (juce::CharPointer_UTF8 (active ? "\xe2\x96\xa0" : "\xe2\x96\xb6")));
        previewButton.setColour (juce::TextButton::buttonColourId,
                                  juce::Colour (active ? 0xffff3b5cu : 0xff2ee86au));   // == kDanger / kPlay
    }

    void refreshGainLabel()
    {
        const double db = juce::Decibels::gainToDecibels ((double) layer.gain, -60.0);
        gainDbLabel.setText (db <= -59.9 ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e dB"))
                                          : juce::String (db, 1) + " dB",
                             juce::dontSendNotification);
    }

    // --- owner's horizontal scrollbar -------------------------------------
    // The waveform's view range expressed in samples, so the thumb's size
    // and position mean something concrete. Disabled at 1x (nothing to
    // scroll), which is also what greys it out.
    void syncScrollBarToView()
    {
        const int total = juce::jmax (1, layer.numFrames());
        const double visible = waveform.getVisibleFraction();
        scrollBarUpdating = true;   // suppress the listener while we set it
        waveScrollBar.setRangeLimits (0.0, (double) total, juce::dontSendNotification);
        waveScrollBar.setCurrentRange (waveform.getViewStartFraction() * total,
                                        visible * total, juce::dontSendNotification);
        scrollBarUpdating = false;
        waveScrollBar.setEnabled (visible < 0.999);
        refreshZoomLabel();
    }

    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override
    {
        if (scrollBarUpdating || bar != &waveScrollBar) return;
        const int total = juce::jmax (1, layer.numFrames());
        waveform.setViewStartFraction (newRangeStart / (double) total);
    }

    // Owner's grid alignment: moves the START MARKER (and with it the grid
    // origin) by a fraction of the current grid step, preserving the region
    // END so nudging never changes the loop's length.
    void nudgeStart (int direction)
    {
        const double step = waveform.gridStepSamples();
        if (step < 1.0) return;
        const bool fine = juce::ModifierKeys::getCurrentModifiers().isShiftDown();
        const int  delta = juce::jmax (1, (int) std::llround (step * (fine ? 0.01 : 0.1))) * direction;

        pushUndoSnapshot();
        const int start  = layer.regionStartClamped();
        const int oldEnd = start + layer.loopLength();
        layer.setRegionStartClamped (start + delta);
        layer.setRegionLengthClamped (juce::jmax (1, oldEnd - layer.regionStartClamped()));
        layer.trimmed = true;
        waveform.repaint();
        if (onLayerEdited) onLayerEdited();
    }

    void refreshZoomLabel()
    {
        zoomLabel.setText (juce::String (juce::roundToInt (waveform.getZoomFactor() * 100.0f)) + "%",
                           juce::dontSendNotification);
    }

    void refreshBpmLabel()
    {
        bpmField.setValue (taggedBpm);   // keeps the scrub field in step with x2 / ÷2 / dialog / undo
        // Owner: no more "tagged" jargon -- this is simply the clip's tempo,
        // detected when it loaded and correctable here.
        bpmLabel.setText ("DETECTED TEMPO", juce::dontSendNotification);
    }

    // Async numeric entry -- runModal() is unavailable in this build (see
    // openClipEditor's own comment); enterModalState + a callback is the
    // available, non-blocking equivalent. deleteWhenDismissed is left false
    // deliberately: that flag deletes the AlertWindow *before* invoking the
    // callback, which would make reading its text editor from the callback a
    // use-after-free. Reading the value first, then deleting explicitly at
    // the end of the callback, avoids that ordering hazard.
    void promptSetBpm()
    {
        auto* aw = new juce::AlertWindow ("Set Tempo", "Enter the clip's tempo (BPM):",
                                           juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("bpm", juce::String (taggedBpm, 1));
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw] (int result)
            {
                if (result == 1)
                {
                    const double v = aw->getTextEditorContents ("bpm").getDoubleValue();
                    if (v > 0.0) { pushUndoSnapshot(); taggedBpm = juce::jlimit (1.0, 999.0, v); refreshBpmLabel(); waveform.setBarGrid (taggedBpm, sampleRate); }
                }
                delete aw;
            }), false);
    }

    // owner's DAW grid selector -- see the gridBox setup in the constructor
    static int divisionsForGridId (int menuId)
    {
        switch (menuId)
        {
            case 1: return 1;    // Bar
            case 2: return 2;    // 1/2
            case 3: return 4;    // 1/4
            case 4: return 8;    // 1/8
            case 5: return 16;   // 1/16
            case 6: return 32;   // 1/32
            default: return 0;   // Off
        }
    }

    // Keeps the button honest about what it will actually do -- "Snap 1/16"
    // when the grid is 1/16, disabled when the grid is off.
    void refreshSnapButtonText()
    {
        const int id = gridBox.getSelectedId();
        const bool on = divisionsForGridId (id) > 0;
        snapToGridButton.setEnabled (on);
        snapToGridButton.setButtonText (on ? ("Snap " + gridBox.getText()) : "Snap");
    }

    juce::Label      nameLabel, statsLabel, bpmLabel, gainDbLabel, gridLabel, zoomLabel;
    ScrubValueLabel  bpmField;   // owner: scrollable/draggable tempo
    juce::TextButton zoomOutButton, zoomInButton, zoomFitButton, gotoStartButton, gotoEndButton;
    // owner: scrollbar when zoomed + grid-alignment nudges
    juce::ScrollBar  waveScrollBar { false };   // false = horizontal
    bool             scrollBarUpdating { false };
    juce::TextButton nudgeLeftButton, nudgeRightButton, alignToGridButton;
    juce::ComboBox   gridBox;
    WaveformView     waveform;
    juce::TextButton previewButton, bpmHalveButton, bpmDoubleButton, setBpmButton,
                     fitFourBarsButton, snapToGridButton, resetMarkersButton,
                     loopPreviewButton, normalizeButton, undoButton, soloPreviewButton,
                     // Editor V2: Redo partners the existing Undo, and Done
                     // is the session-action form of the close affordance the
                     // docked editor already had.
                     redoButton, doneButton;
    // Editor V2: the named control sections and the surfaces drawn behind them.
    juce::Label          tempoSectionLabel, gainSectionLabel, viewSectionLabel;
    juce::Rectangle<int> tempoPanelRect, gainPanelRect, viewPanelRect;
    juce::Slider     gainSlider;
    juce::Colour     accent;   // SPEC_PERFORM_V2 GROUP D: left-edge identity stripe, see paint()
    float            preNormalizeGain { 1.0f };   // owner #2 -- what Normalize-off restores

    // roadmap "Undo for editing operations": snapshots the fields every
    // mutating control in this editor touches (region/fade/gain/tagged
    // tempo), taken right before each edit commits. Capped so an unusually
    // long editing session can't grow this unbounded.
    struct EditSnapshot
    {
        int    regionStart;
        int    regionLength;
        bool   trimmed;
        int    fadeInSamples;
        int    fadeOutSamples;
        float  gain;
        double taggedBpm;
    };
    std::vector<EditSnapshot> undoStack;
    // Editor V2. Redo is the same snapshot type read the other way: undo
    // moves the CURRENT state onto this stack before restoring, so redo can
    // put it back. Cleared by any fresh edit -- a new action makes the
    // branch it would have replayed unreachable, and silently keeping a
    // stale redo is how editors restore audio the user never asked for.
    std::vector<EditSnapshot> redoStack;
    static constexpr size_t kMaxUndoDepth = 20;

    EditSnapshot currentState() const
    {
        return { layer.regionStart, layer.regionLength, layer.trimmed,
                 layer.fadeInSamples, layer.fadeOutSamples, layer.gain, taggedBpm };
    }

    void applySnapshot (const EditSnapshot& snap)
    {
        layer.regionStart    = snap.regionStart;
        layer.regionLength   = snap.regionLength;
        layer.trimmed        = snap.trimmed;
        layer.fadeInSamples  = snap.fadeInSamples;
        layer.fadeOutSamples = snap.fadeOutSamples;
        layer.gain           = snap.gain;
        taggedBpm            = snap.taggedBpm;

        gainSlider.setValue (layer.gain, juce::dontSendNotification);
        refreshGainLabel();
        refreshBpmLabel();
        waveform.setBarGrid (taggedBpm, sampleRate);
        waveform.repaint();
        refreshUndoButtonState();
        if (onLayerEdited) onLayerEdited();
    }

    void pushUndoSnapshot()
    {
        if (undoStack.size() >= kMaxUndoDepth) undoStack.erase (undoStack.begin());
        undoStack.push_back (currentState());
        redoStack.clear();
        refreshUndoButtonState();
    }

    void undoLastEdit()
    {
        if (undoStack.empty()) return;
        const auto snap = undoStack.back();
        undoStack.pop_back();
        redoStack.push_back (currentState());
        applySnapshot (snap);
    }

    void redoLastEdit()
    {
        if (redoStack.empty()) return;
        const auto snap = redoStack.back();
        redoStack.pop_back();
        // Straight onto the undo stack, NOT through pushUndoSnapshot() --
        // that clears the redo stack, which would make a second Redo
        // impossible after the first.
        if (undoStack.size() >= kMaxUndoDepth) undoStack.erase (undoStack.begin());
        undoStack.push_back (currentState());
        applySnapshot (snap);
    }

    void refreshUndoButtonState()
    {
        undoButton.setEnabled (! undoStack.empty());
        redoButton.setEnabled (! redoStack.empty());
    }

    ezdeck::Layer& layer;
    double&        taggedBpm;
    double         sampleRate;

    std::function<void (int, bool, bool)> onPreview;
    std::function<void()>     onStopPreview;
    std::function<bool()>     isPreviewActive;
    std::function<int()>      livePlayhead;   // owner's moving playhead source; see the ctor param
    std::function<void()>     onClose;
    std::function<void()>     onLayerEdited;
    std::function<void (int)> onScrub;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipEditorContent)
};

//==============================================================================
//  PerformLive UI/UX Design Notes (Studio One reference), SPEC_PERFORM_V2
//  GROUP F (103c): the Pad/FX equivalent of ClipEditorContent -- "Deck and FX
//  editing should reuse the exact same editor/styling/workflow as pads." Pads
//  and FX play through ezdeck::OneShotVoice, not ezdeck::Layer, so this binds
//  to a different reference type and has no region/trim/warp concept (one-
//  shots have none), but otherwise mirrors ClipEditorContent's own shell:
//  left-edge accent stripe, 52px header, hosted in the SAME editorView dock
//  (see openVoiceEditor()). Rebind Key/Replace Audio/Clear Slot -- previously
//  showVoiceSlotMenu()'s right-click popup, now removed (task #98) -- are
//  real buttons here instead; the owner (SessionComponent) still implements
//  their actual logic via callbacks, unchanged from that menu's own code.
//
//  Mute/Solo/Trigger Mode bind DIRECTLY to the passed-in OneShotVoice's own
//  enabled/soloed/loop fields (no owner round-trip needed) -- enabled/soloed
//  are real atomics (OneShotVoice.h), safe to touch from this, the message
//  thread, exactly as documented there.
//==============================================================================
class VoiceEditorContent : public juce::Component
{
public:
    VoiceEditorContent (const juce::String& clipName, const juce::String& stats,
                        ezdeck::OneShotVoice& voiceRef,
                        juce::String& nameOverrideRef, bool& colourSetRef, juce::uint32& colourArgbRef,
                        juce::Colour accentColour,
                        std::function<void()> onRebindKeyFn,
                        std::function<void()> onMidiLearnFn,
                        std::function<void()> onReplaceAudioFn,
                        std::function<void()> onClearSlotFn,
                        std::function<void()> onCloseFn,
                        std::function<void (const juce::String&)> onRenamedFn,
                        std::function<void (bool, juce::uint32)> onColourChangedFn)
        : voice (voiceRef), nameOverride (nameOverrideRef), colourSet (colourSetRef), colourArgb (colourArgbRef),
          onRebindKey (std::move (onRebindKeyFn)), onMidiLearn (std::move (onMidiLearnFn)),
          onReplaceAudio (std::move (onReplaceAudioFn)),
          onClearSlot (std::move (onClearSlotFn)), onClose (std::move (onCloseFn)),
          onRenamed (std::move (onRenamedFn)), onColourChanged (std::move (onColourChangedFn))
    {
        accent = accentColour;
        displayName = clipName;

        nameLabel.setText ("EDITING: " + clipName, juce::dontSendNotification);
        nameLabel.setFont (performfonts::headingFont (16.0f));   // editor heading -- Space Grotesk, like every other heading
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));   // == performlive::kTextBright
        addAndMakeVisible (nameLabel);

        statsLabel.setText (stats, juce::dontSendNotification);
        statsLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6f7099u));   // == performlive::kTextFaint
        statsLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (statsLabel);

        // Owner: "2 close signs in the edit window" -- no in-editor Hide
        // button here either; the dock header's × is the one close.

        for (auto* b : { &renameButton, &colourButton, &replaceButton, &rebindButton, &midiLearnButton })
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff151527u));   // == performlive::kCard

        renameButton.setButtonText ("Rename...");
        renameButton.onClick = [this] { promptRename(); };
        addAndMakeVisible (renameButton);

        colourButton.setButtonText ("Colour...");
        colourButton.onClick = [this] { promptColour(); };
        addAndMakeVisible (colourButton);

        // PerformLive UI/UX Design Notes: "Assigned audio" -- read-only
        // display, Replace Audio is how it changes (folded straight in from
        // showVoiceSlotMenu()'s own "Replace Audio..." item).
        replaceButton.setButtonText ("Replace Audio...");
        replaceButton.onClick = [this] { if (onReplaceAudio) onReplaceAudio(); };
        addAndMakeVisible (replaceButton);

        rebindButton.setButtonText ("Rebind Key...");
        rebindButton.onClick = [this] { if (onRebindKey) onRebindKey(); };
        addAndMakeVisible (rebindButton);

        // Owner request: "link to MIDI device from the right-click menu for
        // pads and deck" -- arms the existing MidiActionRouter learn mode
        // (the same one the Settings MIDI tab uses): the next incoming
        // note/CC press on the connected device binds to this slot's
        // trigger action.
        midiLearnButton.setButtonText ("MIDI Learn...");
        midiLearnButton.onClick = [this] { if (onMidiLearn) onMidiLearn(); };
        addAndMakeVisible (midiLearnButton);

        clearButton.setButtonText ("Clear Slot");
        clearButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffff3b5cu));   // == performlive::kDanger, folded from showVoiceSlotMenu()'s own danger-last convention
        clearButton.onClick = [this] { if (onClearSlot) onClearSlot(); if (onClose) onClose(); };
        addAndMakeVisible (clearButton);

        // Mute/Solo -- PerformLive UI/UX Design Notes' explicit per-pad
        // ask. Bind straight to the voice's own atomics (OneShotVoice.h);
        // no owner round-trip needed, unlike name/colour which have no
        // engine-side field to bind to.
        muteButton.setButtonText ("Mute");
        muteButton.setClickingTogglesState (true);
        muteButton.setToggleState (! voice.enabled.load (std::memory_order_relaxed), juce::dontSendNotification);
        muteButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527u));   // == performlive::kCard
        muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffff3b5cu));   // == performlive::kDanger
        muteButton.onClick = [this] { voice.enabled.store (! muteButton.getToggleState(), std::memory_order_relaxed); };
        addAndMakeVisible (muteButton);

        soloButton.setButtonText ("Solo");
        soloButton.setClickingTogglesState (true);
        soloButton.setToggleState (voice.soloed.load (std::memory_order_relaxed), juce::dontSendNotification);
        soloButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527u));   // == performlive::kCard
        soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffc933u));   // == performlive::kQueued
        soloButton.onClick = [this] { voice.soloed.store (soloButton.getToggleState(), std::memory_order_relaxed); };
        addAndMakeVisible (soloButton);

        // Trigger Mode -- binds to the already-real OneShotVoice::loop flag
        // (previously FX-only via onSlotClicked's own toggle-vs-retrigger
        // check; SessionComponent's pad onSlotClicked now checks it too --
        // see openVoiceEditor()'s own comment -- so this control is
        // meaningful for both banks, not pad-inert).
        triggerModeButton.setButtonText (voice.loop ? "Toggle/Hold" : "One-Shot");
        triggerModeButton.setClickingTogglesState (true);
        triggerModeButton.setToggleState (voice.loop, juce::dontSendNotification);
        triggerModeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff151527u));   // == performlive::kCard
        triggerModeButton.setColour (juce::TextButton::buttonOnColourId, accentColour);
        triggerModeButton.onClick = [this]
        {
            voice.loop = triggerModeButton.getToggleState();
            triggerModeButton.setButtonText (voice.loop ? "Toggle/Hold" : "One-Shot");
        };
        addAndMakeVisible (triggerModeButton);

        // Volume -- identical shape to ClipEditorContent's own gainSlider,
        // bound to voice.gain instead of layer.gain (same plain-float,
        // message-thread-write/audio-thread-read shape, same pre-existing
        // data-race category that class's own header already documents).
        gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        gainSlider.setRange (0.0, 2.0, 0.001);
        gainSlider.setValue (voice.gain, juce::dontSendNotification);
        gainSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        gainSlider.setColour (juce::Slider::trackColourId, juce::Colour (0xff2b2b4du));   // == performlive::kBorder
        gainSlider.setColour (juce::Slider::thumbColourId, accentColour);
        gainSlider.onValueChange = [this] { voice.gain = (float) gainSlider.getValue(); refreshGainLabel(); };
        addAndMakeVisible (gainSlider);

        gainDbLabel.setJustificationType (juce::Justification::centredRight);
        gainDbLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));   // == performlive::kTextDim
        addAndMakeVisible (gainDbLabel);
        refreshGainLabel();

        setSize (560, 460);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff14151fu));   // == performlive::kShellBg
        g.fillAll();
        g.setColour (accent);
        g.fillRect (getLocalBounds().removeFromLeft (3));
        g.setColour (juce::Colour (0xff272a3du));   // == performlive::kBorder
        g.drawLine (0.0f, 52.0f, (float) getWidth(), 52.0f, 1.0f);

        // Impact-XT-style large waveform -- static peaks only (OneShotVoice
        // has no region/trim/tempo concept to make an editable, bar-gridded
        // WaveformView meaningful here the way it is for deck layers), read
        // straight from voice.left -- safe: the audio thread only ever
        // READS this vector after load (see loadVoiceClip()), never writes
        // it while playing, so there's no race reading it here for display.
        g.setColour (juce::Colour (0xff151527u));   // == performlive::kCard
        g.fillRoundedRectangle (waveformArea.toFloat(), 6.0f);

        if (voice.left.empty())
        {
            g.setColour (juce::Colour (0xff6f7099u));   // == performlive::kTextFaint
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText ("No audio loaded", waveformArea, juce::Justification::centred, false);
            return;
        }

        const int n = (int) voice.left.size();
        const int w = waveformArea.getWidth();
        const float midY  = (float) waveformArea.getCentreY();
        const float halfH = (float) waveformArea.getHeight() * 0.5f - 6.0f;
        g.setColour (accent.withAlpha (0.85f));
        for (int x = 0; x < w; ++x)
        {
            const int start = (int) ((double) x / (double) w * n);
            const int end   = juce::jmax (start + 1, (int) ((double) (x + 1) / (double) w * n));
            float peak = 0.0f;
            for (int i = start; i < end && i < n; ++i)
                peak = juce::jmax (peak, std::abs (voice.left[(size_t) i]));
            const int px = waveformArea.getX() + x;
            g.drawVerticalLine (px, midY - peak * halfH, midY + peak * halfH);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();

        auto header = area.removeFromTop (52).reduced (12, 4);
        nameLabel.setBounds (header);

        auto statsRow = area.removeFromBottom (24).reduced (12, 2);
        statsLabel.setBounds (statsRow);

        // Two 44px rows of slot-management buttons (Rename/Colour/Replace/
        // Rebind/MIDI Learn, then Mute/Solo/Trigger Mode/Clear) -- same
        // even-width-full-row idiom ClipEditorContent's tempoRow/gainRow use.
        auto row1 = area.removeFromBottom (44).reduced (12, 2);
        const int gap = 8;
        const int w1 = (row1.getWidth() - gap * 4) / 5;
        renameButton.setBounds (row1.removeFromLeft (w1));  row1.removeFromLeft (gap);
        colourButton.setBounds (row1.removeFromLeft (w1));  row1.removeFromLeft (gap);
        replaceButton.setBounds (row1.removeFromLeft (w1)); row1.removeFromLeft (gap);
        rebindButton.setBounds (row1.removeFromLeft (w1));  row1.removeFromLeft (gap);
        midiLearnButton.setBounds (row1);

        area.removeFromBottom (6);

        auto row2 = area.removeFromBottom (44).reduced (12, 2);
        const int w2 = (row2.getWidth() - gap * 3) / 4;
        muteButton.setBounds (row2.removeFromLeft (w2));        row2.removeFromLeft (gap);
        soloButton.setBounds (row2.removeFromLeft (w2));        row2.removeFromLeft (gap);
        triggerModeButton.setBounds (row2.removeFromLeft (w2)); row2.removeFromLeft (gap);
        clearButton.setBounds (row2);

        area.removeFromBottom (6);

        auto gainRow = area.removeFromBottom (44).reduced (12, 2);
        gainDbLabel.setBounds (gainRow.removeFromRight (56));
        gainSlider.setBounds (gainRow.reduced (4, 0));

        area.removeFromBottom (4);
        waveformArea = area;
    }

private:
    void refreshGainLabel()
    {
        const double db = juce::Decibels::gainToDecibels ((double) voice.gain, -60.0);
        gainDbLabel.setText (db <= -59.9 ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e dB"))
                                          : juce::String (db, 1) + " dB",
                             juce::dontSendNotification);
    }

    // Same async-AlertWindow shape ClipEditorContent's own promptSetBpm()
    // uses (runModal() unavailable in this build) -- see that method's own
    // comment for why deleteWhenDismissed stays false.
    void promptRename()
    {
        auto* aw = new juce::AlertWindow ("Rename", "Enter a name for this slot:", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", nameOverride.isNotEmpty() ? nameOverride : displayName);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw] (int result)
            {
                if (result == 1)
                {
                    const auto newName = aw->getTextEditorContents ("name").trim();
                    if (newName.isNotEmpty())
                    {
                        nameOverride = newName;
                        displayName  = newName;
                        nameLabel.setText ("EDITING: " + newName, juce::dontSendNotification);
                        if (onRenamed) onRenamed (newName);
                    }
                }
                delete aw;
            }), false);
    }

    // PerformLive UI/UX Design Notes' own stated default preset list
    // ("purple, blue, red, orange, green, yellow, pink"), same fixed-
    // palette-popup convention as promptColorLayer()/promptColorScene().
    void promptColour()
    {
        static const struct { const char* name; juce::uint32 argb; } kSwatches[] = {
            { "Purple", 0xffa855f7 }, { "Blue",   0xff4d7cff }, { "Red",    0xffff3b5c },
            { "Orange", 0xffff7a1a }, { "Green",  0xff2ee86a }, { "Yellow", 0xffffc933 },
            { "Pink",   0xffff2d95 },
        };
        juce::PopupMenu menu;
        int id = 1;
        for (auto& sw : kSwatches) menu.addItem (id++, sw.name);
        menu.addSeparator();
        const int clearId = id;
        menu.addItem (clearId, "Clear colour", colourSet);

        menu.showMenuAsync (juce::PopupMenu::Options(), [this, clearId] (int result)
        {
            if (result == 0) return;
            if (result == clearId)
            {
                colourSet = false;
                if (onColourChanged) onColourChanged (false, 0);
            }
            else
            {
                const auto& sw = kSwatches[(size_t) (result - 1)];
                colourSet  = true;
                colourArgb = sw.argb;
                if (onColourChanged) onColourChanged (true, sw.argb);
            }
        });
    }

    ezdeck::OneShotVoice& voice;
    juce::String&   nameOverride;
    bool&           colourSet;
    juce::uint32&   colourArgb;
    juce::String    displayName;
    juce::Colour    accent;
    juce::Rectangle<int> waveformArea;

    juce::Label      nameLabel, statsLabel, gainDbLabel;
    juce::TextButton renameButton, colourButton, replaceButton, rebindButton, midiLearnButton, clearButton,
                     muteButton, soloButton, triggerModeButton;
    juce::Slider     gainSlider;

    std::function<void()> onRebindKey, onMidiLearn, onReplaceAudio, onClearSlot, onClose;
    std::function<void (const juce::String&)> onRenamed;
    std::function<void (bool, juce::uint32)>  onColourChanged;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceEditorContent)
};

//==============================================================================
//  Milestone 11: a scene snapshot (PRODUCT_REQUIREMENTS.md §11). Deliberately
//  small -- signature + one active deck slot + bpm + that deck's own 4 tab
//  flags + which of the 12 pads were active, matching PRD §11's own field
//  list read literally (see project/MILESTONE_11-15_ARCHITECTURE.md's
//  Research findings for the exact interpretation of "side/active row key"
//  and "per-row tempo" against this port's model). Not JUCE-free (uses
//  juce::String) -- this is app state, not engine state.
//==============================================================================
struct Scene
{
    bool         filled { false };
    juce::String name;
    int          signatureIndex { 0 };
    int          activeSlot     { 0 };   // 0..kDecksPerSignature-1, within signatureIndex
    double       bpm { 120.0 };
    std::array<bool, ezdeck::kNumLayers> tabEnabled { true, true, true, true,
                                                      true, true, true, true };
    std::array<bool, 12> padActive {};

    // SPEC_PERFORM_V2 GROUP E: owner-assignable colour ("lights up like a
    // MIDI keyboard"). Independent of filled/name -- set once via the
    // scene's own context menu, survives Save current here / Rename.
    bool         colourSet  { false };
    juce::uint32 colourArgb { 0 };
};

//==============================================================================
//  PX-001: PerformLive Application Shell design tokens.
//  Applied only to shell chrome this component creates directly (header,
//  nav, scene bar, signature rail, transport bar) via per-component
//  setColour() calls -- never through a global LookAndFeel, since that would
//  cascade into Pads/FX/Mixer/Launch-lane, which this sprint must not touch
//  ("the deck workspace should intentionally still look old").
//==============================================================================
namespace performlive
{
    constexpr uint32_t kShellBg     = 0xff0c0c17;   // shell chrome background, slightly darker than content (0xff141422)
    constexpr uint32_t kCard        = 0xff151527;   // header/nav/rail/transport "card" surfaces
    constexpr uint32_t kBorder      = 0xff2b2b4d;   // soft borders
    constexpr uint32_t kIndigo      = 0xff7c5cff;   // primary accent (active nav/rail/transport)
    constexpr uint32_t kIndigoDim   = 0xff3a2f6b;   // indigo, dimmed -- inactive-but-present chrome
    constexpr uint32_t kPurple      = 0xffa855f7;   // secondary highlight (Play button)
    constexpr uint32_t kTextBright  = 0xfff2f0ff;
    constexpr uint32_t kTextDim     = 0xffa3a6cc;   // matches the app's existing dim-text colour exactly

    // ---- UI_SPEC_PERFORM.md §1: new -- deeper workspace background ----
    constexpr uint32_t kWorkspaceBg = 0xff07070f;   // behind the deck grid
    constexpr uint32_t kSlotEmpty   = 0xff0f0f1d;   // empty deck card fill
    constexpr uint32_t kTextFaint   = 0xff6f7099;   // metadata, units, labels

    // ---- UI_SPEC_PERFORM.md §1: new -- per-column accents (deck 1..4) ----
    constexpr uint32_t kCol1        = 0xff00d9ff;   // cyan
    constexpr uint32_t kCol2        = 0xffa855f7;   // violet
    constexpr uint32_t kCol3        = 0xffff2d95;   // magenta
    constexpr uint32_t kCol4        = 0xffffa62b;   // gold
    // PX-B: layers 5-8. Continues the section wheel rather than inventing a
    // second palette, and stays clear of the semantic three (play green,
    // queued amber, danger red) so no lane can be mistaken for a state.
    constexpr uint32_t kCol5        = 0xff3dffc0;   // aqua
    constexpr uint32_t kCol6        = 0xff4d7cff;   // electric blue
    constexpr uint32_t kCol7        = 0xffb6ff2e;   // lime
    constexpr uint32_t kCol8        = 0xffff5c3b;   // coral

    // ---- UI_SPEC_PERFORM.md §1: new -- semantic ----
    constexpr uint32_t kPlay        = 0xff2ee86a;   // transport play / live
    constexpr uint32_t kQueued      = 0xffffc933;   // pending switch
    constexpr uint32_t kDanger      = 0xffff3b5c;

    // ---- UI_SPEC_MIXER.md §1: new -- meter gradient stops ----
    // (MixerChannelStrip itself is defined earlier in the file than this
    // namespace, so it uses the identical raw hex literals directly --
    // same convention as columnAccent()'s own values above.)
    constexpr uint32_t kMeterLow    = 0xff2ee86a;   // green (same as kPlay)
    constexpr uint32_t kMeterMid    = 0xffffc933;   // amber (same as kQueued)
    constexpr uint32_t kMeterHot    = 0xffff3b5c;   // red   (same as kDanger)

    // ---- neon accents shared by the PERFORM cards and the PLAYBACK HUD ----
    constexpr uint32_t kMagenta     = 0xffff2d95;   // the hottest accent in the set
    constexpr uint32_t kCyan        = 0xff00d9ff;

    /** Paints a soft outer halo around a rounded rect.

        JUCE has no blur, so the halo is four concentric rounded rects at
        falling alpha -- cheap, and at these radii indistinguishable from a
        real blur. The CALLER must leave margin for it: drawing this at the
        very edge of a component clips the halo away entirely and the
        component simply looks unlit (which is exactly the bug the deck
        cards had -- their bounds were inset 0.5px and every ring landed
        outside getLocalBounds()).
    */
    inline void neonGlow (juce::Graphics& g, juce::Rectangle<float> r, float radius,
                          juce::Colour c, float strength = 1.0f, int rings = 4)
    {
        for (int i = rings; i >= 1; --i)
        {
            const float expand = (float) i * 1.6f;
            g.setColour (c.withAlpha (juce::jlimit (0.0f, 1.0f, 0.16f * strength / (float) i)));
            g.fillRoundedRectangle (r.expanded (expand), radius + expand);
        }
    }

    inline juce::Colour columnAccent (int col)
    {
        static const uint32_t c[8] { kCol1, kCol2, kCol3, kCol4,
                                     kCol5, kCol6, kCol7, kCol8 };
        return juce::Colour (c[juce::jlimit (0, 7, col)]);
    }
}

//==============================================================================
//  Phase 1.1 P2/P3: a real, self-painting background for the Library/Mixer
//  dock containers. The FIRST version of docking (Phase 1.1 P2-a) instead
//  had SessionComponent's own paint() fillRect() a plain juce::Component's
//  bounds -- caught live during this session's own screenshot verification
//  as a real bug: maximizing a dock left thin strips of stale PERFORM
//  content visible in the gaps between mixer strips/library cards.
//  toFront()/repaint() calls in resized() didn't fix it (verified -- both
//  tried first, neither changed the result), which makes sense once you
//  stop assuming it's a z-order/dirty-region problem: a plain Component has
//  NOTHING of its own to paint, so relying on the PARENT to separately
//  fillRect() a rectangle that must exactly track a CHILD's current bounds,
//  every frame, forever, is fragile by construction -- any place those two
//  ever disagree (even briefly, mid-resize) shows through. A self-painting
//  Component fixes the class of bug, not just this one instance of it: its
//  own background is always correctly clipped to its own current bounds,
//  as part of its own paint cycle, with no separate coordinate to keep in
//  sync.
//==============================================================================
class DockShellView : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0c0c17u));   // == performlive::kShellBg
    }
};

//==============================================================================
//  Owner bug report ("there should be a scroll so that when the library or
//  mixer is on I can still access the pads down below"): the PERFORM content
//  now lives inside a juce::Viewport, so when a dock (or a short window)
//  squeezes it below its own minimum layout height, a vertical scrollbar
//  appears instead of content being covered or clipped away.
//
//  That forced the perform-area background painting to move here from
//  SessionComponent::paint(): those fills used SessionComponent-absolute
//  rects, which can't follow the content as it scrolls -- a component that
//  paints its OWN backgrounds in its OWN local space scrolls correctly by
//  construction (the same reasoning as DockShellView's own class comment).
//  The rects are set by SessionComponent::resized() in THIS component's
//  local coordinates. Raw hex, not performlive:: -- this class is defined
//  before that namespace exists in this file (established convention).
//==============================================================================
//==============================================================================
//  Owner: "something on the left side of the decks that shows this one has a
//  click and a guide, and in perform mode you trigger it." One cell per row,
//  two pills: CLICK and GUIDE. Dashed = the row has no such track; outlined
//  = it has one, switched off; filled = on. A small word under the label says
//  whether it's the song's own track or the app's built-in one. Tap a pill
//  to switch it; right-click or press-and-hold for the menu.
//==============================================================================
class GuideSlotCell : public juce::Component, public juce::SettableTooltipClient
{
public:
    GuideSlotCell()
    {
        touchHold.onLongPress = [this] (juce::Point<int>) { if (onMenu) onMenu(); };
    }

    std::function<void (bool isGuide)> onTapPill;
    std::function<void()> onMenu;

    void setState (bool hasClickTrack, bool clickOn, bool hasGuideTrack, bool guideOn, bool songClick, bool songGuide)
    {
        hasClick = hasClickTrack; clickLit = clickOn; hasGuide = hasGuideTrack; guideLit = guideOn;
        useSongClick = songClick; useSongGuide = songGuide;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour amber (0xffffc933), cyan (0xff00d9ff), card (0xff151527), dim (0xff6f7099), bright (0xfff2f0ff);
        auto area = getLocalBounds().reduced (2);
        const int gap = 4;
        const int h = (area.getHeight() - gap) / 2;
        drawPill (g, area.removeFromTop (h).toFloat(), "CLICK", amber, hasClick, clickLit, useSongClick, card, dim, bright);
        area.removeFromTop (gap);
        drawPill (g, area.toFloat(), "GUIDE", cyan, hasGuide, guideLit, useSongGuide, card, dim, bright);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onMenu) onMenu(); return; }
        touchHold.begin (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { touchHold.drag (e); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (touchHold.end() || e.mods.isPopupMenu()) return;
        if (! getLocalBounds().contains (e.getPosition())) return;
        if (onTapPill) onTapPill (e.getPosition().y > getHeight() / 2);
    }

private:
    static void drawPill (juce::Graphics& g, juce::Rectangle<float> r, const char* label, juce::Colour accent,
                          bool has, bool lit, bool song, juce::Colour card, juce::Colour dim, juce::Colour bright)
    {
        const float radius = 6.0f;
        if (has && lit)
        {
            g.setColour (accent.withAlpha (0.10f));
            g.fillRoundedRectangle (r.expanded (2.0f), radius + 2.0f);
            g.setColour (accent.withAlpha (0.85f));
            g.fillRoundedRectangle (r, radius);
        }
        else
        {
            g.setColour (card);
            g.fillRoundedRectangle (r, radius);
            if (has) { g.setColour (accent.withAlpha (0.8f)); g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.5f); }
            else
            {
                juce::Path outline, dashed;
                outline.addRoundedRectangle (r.reduced (0.5f), radius);
                const float dashes[] { 3.0f, 3.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
                g.setColour (dim.withAlpha (0.6f));
                g.strokePath (dashed, juce::PathStrokeType (1.0f));
            }
        }

        auto text = r.reduced (2.0f, 3.0f);
        g.setColour (has && lit ? juce::Colours::black : (has ? bright : dim));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)).withExtraKerningFactor (0.06f));
        const bool room = r.getHeight() >= 30.0f;
        g.drawText (label, room ? text.removeFromTop (text.getHeight() * 0.55f) : text, juce::Justification::centred, false);
        if (room)
        {
            g.setFont (juce::Font (juce::FontOptions (8.0f)));
            g.setColour (has && lit ? juce::Colours::black.withAlpha (0.7f) : dim);
            g.drawText (! has ? "none" : (song ? "song" : "built-in"), text, juce::Justification::centred, false);
        }
    }

    bool hasClick { false }, clickLit { false }, hasGuide { false }, guideLit { false };
    bool useSongClick { true }, useSongGuide { true };
    eztouch::LongPress touchHold;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GuideSlotCell)
};

class PerformContentView : public juce::Component
{
public:
    juce::Rectangle<int> sceneBarBgArea, railBgArea, guideColBgArea, deckPanelArea, sceneLabelArea;

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff0c0c17));   // == performlive::kShellBg
        g.fillRect (sceneBarBgArea);

        // the song-tracks column between the rail and the decks (GuideSlotCell)
        g.setColour (juce::Colour (0xff0c0c17));
        g.fillRect (guideColBgArea);
        g.setColour (juce::Colour (0xffa3a6cc));
        g.setFont (performfonts::headingFont (9.0f).withExtraKerningFactor (0.01f));
        g.drawFittedText ("SONG\nTRACKS", guideColBgArea.reduced (6, 18).removeFromTop (24),
                           juce::Justification::centred, 2);

        g.setColour (juce::Colour (0xffa3a6cc));   // == performlive::kTextDim
        g.setFont (performfonts::headingFont (11.0f).withExtraKerningFactor (0.01f));
        g.drawText ("SCENES", sceneLabelArea, juce::Justification::centredLeft, false);

        // vertical signature rail's card background -- the buttons
        // themselves are real components, positioned in resized()
        g.setColour (juce::Colour (0xff151527));   // == performlive::kCard
        g.fillRect (railBgArea);
        g.setColour (juce::Colour (0xffa3a6cc));
        g.setFont (performfonts::headingFont (10.0f).withExtraKerningFactor (0.01f));
        g.drawFittedText ("TIME\nSIGNATURE", railBgArea.reduced (8, 6).removeFromTop (24),
                           juce::Justification::centredLeft, 2);

        // the deck-grid workspace panel background
        g.setColour (juce::Colour (0xff07070f));   // == performlive::kWorkspaceBg
        g.fillRoundedRectangle (deckPanelArea.toFloat(), 10.0f);
    }
};

class SessionComponent : public juce::AudioAppComponent,
                          public ezplayback::PlaybackHost,     // the PLAYBACK view reads/drives the engine through this
                          public juce::DragAndDropContainer,   // Phase 1.1 P1 "Deck Loading": Library -> deck drag/drop
                          public juce::FileDragAndDropTarget,  // owner: "the whole app should accept drag and drop from Windows" -- generic fallback, see filesDropped()
                          private juce::Button::Listener,
                          private juce::ChangeListener,        // audio device swapped/plugged in -- see requestAllOutputChannels()
                          private juce::Timer
{
public:
    // Milestone 6: kNumDecks is now the FLAT index space across every
    // signature bank (SignatureManager::kTotalDecks -- 7 signatures x 8
    // decks each, ARCHITECTURE.md's resolved Architecture Decision Pending
    // #1). kNumSlots is the fixed number of persistent UI widgets shown at
    // once -- one signature's worth (8) -- re-bound to whichever flat deck
    // indices the currently VIEWED signature owns, via flatDeckIndexForSlot()
    // below, rather than recreating widgets on every signature switch.
    static constexpr int kNumDecks = ezdeck::SignatureManager::kTotalDecks;
    static constexpr int kNumSlots = ezdeck::kDecksPerSignature;

    // UI_SPEC_PERFORM.md layout constants -- literal spec dimensions
    // (§2.1/§2.2/§2.3/§2.6). Steps 4/6 replace PX-001's own placeholder
    // values (170/64/40/32) with the spec's real ones: header now includes
    // the nav tabs inline (56px total, not a separate 40px row), transport
    // and scenes merge into one 64px row directly under the header, and the
    // rail shrinks to the spec's actual 88px.
    static constexpr int kHeaderHeight          = 56;    // §2.1 -- brand, nav tabs, signature indicator, master volume, project selector, gear all in this one row
    static constexpr int kTransportSceneHeight  = 64;    // §2.2 -- transport (left, fixed 420px) + scenes (right, flex), one row
    static constexpr int kRailWidth             = 88;    // §2.3
    static constexpr int kGuideColWidth         = 68;    // the song-tracks (click/guide) column left of the decks
    struct SongTrack;   // a row's own click or guide track -- defined with the song-track members below
    static constexpr int kStatusBarHeight       = 26;    // §2.6 -- new this step

    // (The old kMinPerformPanelHeight dock-clamp constant is gone: PERFORM
    // scrolls now -- see requiredPerformHeight() and PerformContentView --
    // so a dock squeezing it can never make content overlap or vanish, and
    // nothing needs to cap the dock's height anymore.)

    SessionComponent()
    {
        formatManager.registerBasicFormats();
        session.setTempo (masterTempo);
        loadCueBank();   // Guide.h: the spoken cue recordings, once
        for (auto& c : trackInput) c.store (-1, std::memory_order_relaxed);
        if (auto* settings = getAppSettings()) pluginLibrary.loadFrom (*settings);
        for (auto& s : instruments) s.setPlayHead (&hostPlayHead);
        for (int i = 0; i < kNumStrips; ++i)
        {
            strips[(size_t) i] = std::make_unique<amanorsac::perform::PerformProcessor>();
            strips[(size_t) i]->setPlayConfigDetails (2, 2, 48000.0, 512);
            stripOn[(size_t) i].store (false, std::memory_order_relaxed);
        }

        // SPEC_PERFORM_V2 GROUP H4: -1 = "no link" for every layer -- see
        // layerLinkedPad's own declaration comment for why this can't be a
        // plain `{}` member initializer.
        for (auto& row : layerLinkedPad) row.fill (-1);

        // SPEC_PERFORM_V2 GROUP E: "replace the text block with a logo
        // image the owner will supply; leave a correctly-sized placeholder
        // that swaps to the image when provided." searchFor() is this
        // file's own established 3-candidate lookup (already used for
        // pad/fx/project-file assets) -- no logo.png exists yet in this
        // environment, so this deliberately falls through to paint()'s own
        // placeholder every time until the owner hands off the file.
        {
            // Owner: the header read "LOGO / AQUARII AUDIO" whenever logo.png
            // wasn't beside the exe. The wordmark is embedded now (CMake
            // PerformLiveArt); a logo.png next to the exe still overrides it.
            auto logoFile = searchFor ("logo.png");
            if (logoFile.existsAsFile())
                logoImage = juce::ImageFileFormat::loadFrom (logoFile);
            if (! logoImage.isValid())
                logoImage = juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize);
        }

        // Milestone 16: the default library root (MILESTONE_16_ARCHITECTURE.md's
        // own storage layout, now Documents/Amanorsac Studio/PerformLive/Library per the
        // studio's File & Data Conventions). Created if
        // missing; addRoot() itself handles "not found yet" gracefully
        // (load() returns false for a fresh library, not an error).
        libraryManager.addRoot (productpaths::userContent().getChildFile ("Library"));

        // Milestone 6: give every deck its own signature's structural beat
        // count (Deck::beatsPerBar) -- a one-way, downward write; Session
        // never queries SignatureManager back, per Architecture Decision
        // Pending #1's resolution.
        for (int sigIdx = 0; sigIdx < signatureManager.size(); ++sigIdx)
        {
            const auto& sig = signatureManager.signature (sigIdx);
            for (int slot = 0; slot < kNumSlots; ++slot)
            {
                auto& d = session.decks[(size_t) (sig.firstDeckIndex + slot)];
                d.beatsPerBar = sig.beatsPerBar;
                d.mode = ezdeck::DeckMode::stem;   // owner: every row starts in stem mode (a project can still save loop mode)
                useSongClick[(size_t) (sig.firstDeckIndex + slot)] = true;
                useSongGuide[(size_t) (sig.firstDeckIndex + slot)] = true;
                liveClickTrack[(size_t) (sig.firstDeckIndex + slot)].store (nullptr, std::memory_order_relaxed);
                liveGuideTrack[(size_t) (sig.firstDeckIndex + slot)].store (nullptr, std::memory_order_relaxed);
                // bounds nextPlay's auto-advance to this signature's own
                // 8-deck range -- see Deck.h's own comment on this field.
                d.autoAdvanceRangeStart = sig.firstDeckIndex;
                d.autoAdvanceRangeCount = kNumSlots;
            }
        }

        // Ship-empty (owner direction: "delete all imported files, make the
        // app empty"): the a1.wav/b1.wav dev-fixture autoload that seeded
        // signature 0's first 2 decks since Milestone 6 is gone -- every
        // deck now starts unloaded and the ONLY ways audio gets in are the
        // user's own actions (Library load, drag-drop, Replace Audio,
        // project open). findLayerFile()/loadLayer() themselves stay: the
        // project loader and every runtime load path still use them.

        // UI_SPEC_PERFORM.md §2.4/§3 (build-order step 3): the real deck
        // grid. deckCards[slot][l]'s gestures resolve flatDeckIndexForSlot()
        // at CLICK TIME, not here -- the same established pattern
        // layerToggles/triggerButtons used, so switching viewedSignature or
        // the A/B bank never requires recreating these 32+8 widgets.
        for (int slot = 0; slot < kNumSlots; ++slot)
        {
            for (int l = 0; l < ezdeck::kNumLayers; ++l)
            {
                auto* card = new DeckCard();
                card->setAccent (performlive::columnAccent (l));

                // Tap: arm/disarm this layer (existing enabled flag -- the
                // SAME single-line mutation the old LayerToggle checkbox
                // performed, not a second implementation of it).
                // SPEC_PERFORM_V2 GROUP B: "an empty deck's + (and clicking
                // an empty deck) opens the Library to load a loop into that
                // slot" -- replaces the old honest "no clip loader yet"
                // toast, which was true when it was written but is the bug
                // this group fixes. Arms pendingLoadDeck/Layer so the very
                // next Library card tap loads here instead of auditioning
                // (see loadAssetIntoDeckSlot() and MySamplesTab's
                // onCardTapMaybeLoad wiring) -- the SAME underlying load
                // path drag-drop already uses just below, not a second one.
                card->onTap = [this, slot, l]
                {
                    const int flat = flatDeckIndexForSlot (slot);
                    auto& layer = session.decks[(size_t) flat].layers[(size_t) l];
                    if (! layer.loaded)
                    {
                        pendingLoadDeck = flat;
                        pendingLoadLayer = l;
                        libraryDockOpen = true;
                        mixerDockOpen = false;
                        closeEditorDock();
                        refreshActiveView();
                        resized();
                        showToast (deckLabel (flat) + " layer " + juce::String (l + 1) + ": tap a Library sample to load it here");
                        return;
                    }
                    layer.enabled = ! layer.enabled.load();
                };
                // Double-tap: existing openClipEditor(), unchanged.
                card->onDoubleTap = [this, slot, l]
                {
                    const int flat = flatDeckIndexForSlot (slot);
                    if (session.decks[(size_t) flat].layers[(size_t) l].loaded)
                        openClipEditor (flat, l);
                };
                // Long-press / right-click / "..." glyph: Phase 1.1 P1's new
                // per-layer "Deck Management" menu (Rename/Replace/Clear/
                // Duplicate) -- previously opened showDeckTempoMenu() (the
                // ROW-level tempo/mode menu), which stays reachable from the
                // trigger cell's own onMenu just below, unchanged.
                card->onMenu = [this, slot, l] { showLayerContextMenu (flatDeckIndexForSlot (slot), l); };

                // Phase 1.1 P1 "Deck Loading": drag a stem/loop from the
                // Library straight onto this layer card. "Packs" (the
                // brief's third drag source) don't exist as a distinct
                // draggable unit yet -- the Library has no auto-grouped
                // stem-collection concept until Phase 1.1 Priority 2 builds
                // it (this task's own scope is Priority 1's "Deck Loading"
                // bullet, not Library's data model) -- flagged, not silently
                // dropped. A single sample/loop drag works today via the
                // same LibrarySampleCard vocabulary any future pack-level
                // drag source would also need to speak.
                card->onAssetDropped = [this, slot, l] (const juce::String& assetId)
                {
                    loadAssetIntoDeckSlot (flatDeckIndexForSlot (slot), l, assetId);
                };

                // Owner request ("the whole app should accept drag and drop
                // from Windows"): an OS audio file dropped on this card
                // loads here in place (referencing, same as every load),
                // then offers to add it to the Library too.
                card->onFilesDropped = [this, slot, l] (const juce::StringArray& paths)
                {
                    const int flat = flatDeckIndexForSlot (slot);
                    if (! canMutateDeckState (flat)) { showToast (deckLabel (flat) + ": stop this deck first to load into it"); return; }
                    juce::String firstAudio;
                    juce::Array<juce::File> audio;
                    for (const auto& p : paths) if (isSupportedAudioPath (p)) { audio.add (juce::File (p)); if (firstAudio.isEmpty()) firstAudio = p; }
                    if (firstAudio.isEmpty()) return;
                    // Owner: several files at once are a set of stems -- the
                    // import window reads their names and places them
                    if (audio.size() > 1) { openStemImport (flat, audio, {}); return; }
                    clearLayer (flat, l);
                    loadLayer (flat, l, juce::File (firstAudio));
                    refreshSlotLabels();
                    showToast (deckLabel (flat) + " layer " + juce::String (l + 1) + ": loaded " + juce::File (firstAudio).getFileName());
                    repaint();
                    offerLibraryImport (paths);
                };

                performView.addAndMakeVisible (card);
                deckCards[(size_t) slot].add (card);
            }

            auto* cell = new DeckTriggerCell();
            // Live-usage correction (post-GROUP A): the "trigger click only
            // selects/arms while stopped, transport Play separately starts
            // it" split above tested as confusing in practice -- clicking
            // A1/A2 appeared to do nothing (no sound), which read as broken,
            // not as "armed." Live playback/DJ rigs (and this deck grid's own
            // ActionId::PlayNext, just below) treat a trigger as "play this,
            // now" unconditionally, so the trigger cell now matches: switch
            // (immediate if stopped, bar-quantized if already running, same
            // as before) AND force transportRunning true either way. The
            // active deck's own unmuted (Layer::enabled) layers are what
            // actually render once transportRunning flips -- no separate
            // "trigger all unmuted decks" step needed, that's just normal
            // per-layer mute already governing playback.
            cell->onTrigger = [this, slot] { triggerRowSlot (slot); };
            cell->onMenu    = [this, slot] { showDeckTempoMenu (flatDeckIndexForSlot (slot)); };
            // Owner #9: the mode pill flips stem/loop -- the exact same flip
            // (and canMutateDeckState gate) as the row menu's "Switch to
            // stem/loop mode" item, just one tap closer.
            cell->onModeToggle = [this, slot]
            {
                const int flat = flatDeckIndexForSlot (slot);
                if (! canMutateDeckState (flat)) { showToast (deckLabel (flat) + ": stop this deck first to switch mode"); return; }
                auto& d = session.decks[(size_t) flat];
                d.mode = (d.mode == ezdeck::DeckMode::stem) ? ezdeck::DeckMode::loop : ezdeck::DeckMode::stem;
                showToast (deckLabel (flat) + ": switched to " + (d.mode == ezdeck::DeckMode::stem ? "stem" : "loop") + " mode");
                refreshSlotLabels();
                repaint();
            };
            cell->setTooltip ("Play this row now");
            performView.addAndMakeVisible (cell);
            triggerCells.add (cell);

            // the row's click and guide, left of its decks
            auto* gcell = new GuideSlotCell();
            gcell->onTapPill = [this, slot] (bool isGuide) { toggleSongTrackPill (flatDeckIndexForSlot (slot), isGuide); };
            gcell->onMenu    = [this, slot] { showSongTrackMenu (flatDeckIndexForSlot (slot)); };
            gcell->setTooltip ("This row's click and guide: tap to switch on or off, hold or right-click for options");
            performView.addAndMakeVisible (gcell);
            guideCells.add (gcell);
        }

        // UI_SPEC_PERFORM.md §2.4: the panel chrome around the grid above.
        // UI_SPEC_PERFORM.md §7 (build-order step 7): re-parented into
        // performView -- this whole panel is PERFORM-only content now.
        deckPanelLabel.setText ("DECKS", juce::dontSendNotification);
        // SPEC_PERFORM_V2 GROUP F (105): same kerning treatment as every
        // other uppercase panel title -- see DockHeaderBar's own.
        deckPanelLabel.setFont (performfonts::headingFont (11.0f).withExtraKerningFactor (0.01f));
        deckPanelLabel.setColour (juce::Label::textColourId, juce::Colour (performlive::kTextDim));
        performView.addAndMakeVisible (deckPanelLabel);

        deckBankLabel.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::plain)));
        deckBankLabel.setColour (juce::Label::textColourId, juce::Colour (performlive::kTextFaint));
        performView.addAndMakeVisible (deckBankLabel);

        bankAButton.setButtonText ("A");
        // SPEC_PERFORM_V2 GROUP F: "A/B button not working well" -- root
        // cause found by live reproduction, not by reading this code (it
        // looked correct on paper). resized() computes each slot's row via
        // `slot - deckBank * 4` (see this class's own resized()), so
        // switching banks changes which physical row every one of the 8
        // slot widgets belongs in -- exactly like showAllRowsToggle's own
        // handler below, which already (correctly) calls resized(). This
        // handler and bankBButton's own were the only two that didn't.
        bankAButton.onClick = [this] { deckBank = 0; refreshDeckPanelChrome(); refreshSlotLabels(); resized(); };
        bankAButton.setLookAndFeel (&tactileButtonLookAndFeel());
        bankAButton.setTooltip ("Rows A1-A4");
        performView.addAndMakeVisible (bankAButton);

        bankBButton.setButtonText ("B");
        bankBButton.onClick = [this] { deckBank = 1; refreshDeckPanelChrome(); refreshSlotLabels(); resized(); };
        bankBButton.setLookAndFeel (&tactileButtonLookAndFeel());
        bankBButton.setTooltip ("Rows B1-B4");
        performView.addAndMakeVisible (bankBButton);

        // Chevrons rather than words, per the owner: the control says which
        // direction it moves and the dots say where you are.
        pagePrevButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xb9")));
        pagePrevButton.onClick = [this] { setDeckPage (deckPage - 1); };
        pagePrevButton.setLookAndFeel (&tactileButtonLookAndFeel());
        pagePrevButton.setTooltip ("Decks 1-4");
        performView.addAndMakeVisible (pagePrevButton);

        pageNextButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xba")));
        pageNextButton.onClick = [this] { setDeckPage (deckPage + 1); };
        pageNextButton.setLookAndFeel (&tactileButtonLookAndFeel());
        pageNextButton.setTooltip ("Decks 5-8");
        performView.addAndMakeVisible (pageNextButton);

        pageDotsLabel.setJustificationType (juce::Justification::centred);
        pageDotsLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::plain)));
        pageDotsLabel.setInterceptsMouseClicks (false, false);
        performView.addAndMakeVisible (pageDotsLabel);

        // "SHOW ALL (8 ROWS)" changes row COUNT (4 <-> 8), which changes
        // card heights/positions -- unlike the A/B bank switch above, this
        // needs a real resized(), not just a repaint of already-placed widgets.
        showAllRowsToggle.setButtonText ("SHOW ALL (8 ROWS)");
        showAllRowsToggle.onClick = [this]
        {
            showAllDeckRows = showAllRowsToggle.getToggleState();
            refreshDeckPanelChrome();
            refreshSlotLabels();
            resized();
        };
        performView.addAndMakeVisible (showAllRowsToggle);

        for (int c = 0; c < kNumCols; ++c)
        {
            deckColumnHeaders[(size_t) c].setText ("DECK " + juce::String (c + 1), juce::dontSendNotification);
            deckColumnHeaders[(size_t) c].setJustificationType (juce::Justification::centred);
            // SPEC_PERFORM_V2 GROUP F (105): same kerning treatment as
            // every other uppercase header -- see DockHeaderBar's own.
            deckColumnHeaders[(size_t) c].setFont (performfonts::headingFont (10.0f).withExtraKerningFactor (0.01f));
            deckColumnHeaders[(size_t) c].setColour (juce::Label::textColourId,
                                                      performlive::columnAccent (c).withAlpha (0.70f));
            performView.addAndMakeVisible (deckColumnHeaders[(size_t) c]);
        }

        refreshDeckPanelChrome();

        // Milestone 6: the signature rail (PRD §1's left-hand rail) --
        // switching it only repoints which flat deck range the 8 widgets
        // above display/control (refreshSlotLabels(), buttonClicked()); it
        // never touches playback state, matching PRD §1's own Edge Cases
        // ("switching signatures... only the visible grid changes").
        for (int i = 0; i < signatureManager.size(); ++i)
        {
            // Phase 1.1 P1: SignatureRailButton (glow/press-feedback), not a
            // plain juce::TextButton -- see that class's own comment. Not a
            // juce::Button, so it can't go through buttonClicked()'s
            // Listener mechanism the rest of this constructor uses; wired
            // via its own onTap instead, same pattern DeckTriggerCell/
            // DeckCard already use for their own gestures.
            auto* b = new SignatureRailButton();
            b->setLabel (signatureManager.signature (i).name);
            b->onTap = [this, i] { viewedSignature = i; refreshSlotLabels(); repaint(); };
            // SPEC_PERFORM_V2 GROUP E: "same treatment [as scenes] --
            // owner-assignable colour, lit/glowing when active."
            b->onRightClick = [this, i] { promptColorSignature (i); };
            if (signatureColourSet[(size_t) i]) b->setAccentColour (juce::Colour (signatureColourArgb[(size_t) i]));
            b->setTooltip ("View " + juce::String (signatureManager.signature (i).name) + juce::String (juce::CharPointer_UTF8 (" decks " "\xc2" "\xb7" " right-click to recolour")));
            performView.addAndMakeVisible (b);
            signatureButtons.add (b);
        }

        // UI_SPEC_PERFORM.md §2.3: the rail's own "+" button. Adding a real
        // custom signature needs runtime signature authoring that doesn't
        // exist anywhere in this app (NEXT_STEPS.md #12's own established
        // deferral, already noted for "+ add a custom signature" since
        // Milestone 6) -- present, dashed, honestly inert with a toast,
        // never a fabricated new signature.
        // (The signature rail's "+" honest-no-op is gone -- owner #15: "the
        // + sign by the time signatures is not working, take it off.")

        // UI_SPEC_PERFORM.md §2.2: the scene group's own "+". scenes is a
        // fixed std::array<Scene, 8> (Milestone 11) -- there is no 9th slot
        // (The scene bar's "+" honest-no-op button is gone -- owner: "the
        // plus sign by the scenes is useless, take it off." The engine has
        // 8 fixed scene slots; a button that can only toast that fact adds
        // clutter, not function. The scene buttons absorb its width.)

        refreshSlotLabels();

        tapButton.setButtonText ("TAP");
        tapButton.addListener (this);
        tapButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        tapButton.setLookAndFeel (&tactileButtonLookAndFeel());
        tapButton.setTooltip ("Tap tempo");
        performView.addAndMakeVisible (tapButton);

        // UI_SPEC_PERFORM.md §2.2: LOCK off = kCard/kTextDim, on = kIndigo/kTextBright.
        lockToggle.setButtonText ("LOCK");
        lockToggle.setClickingTogglesState (true);
        lockToggle.addListener (this);
        lockToggle.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        lockToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (performlive::kIndigo));
        lockToggle.setColour (juce::TextButton::textColourOffId, juce::Colour (performlive::kTextDim));
        lockToggle.setColour (juce::TextButton::textColourOnId, juce::Colour (performlive::kTextBright));
        lockToggle.setLookAndFeel (&tactileButtonLookAndFeel());
        lockToggle.setTooltip ("Warp every row to the master tempo");
        performView.addAndMakeVisible (lockToggle);

        // Milestone 13: opens the Settings panel (PRD §14's "Options modal").
        // PX-001: relocated into the header as a gear icon -- same action,
        // new position/appearance only. (The U+2699 glyph itself falls back
        // to a dot-cluster in the available font -- a known, previously
        // reported gap, not new this step.)
        // SPEC_PERFORM_V2 GROUP F (104): U+2699 GEAR was a font glyph this
        // app's chosen font doesn't actually contain, falling back to a
        // dot-cluster -- IconGlyphButton draws a real vector gear instead.
        settingsButton.onClick = [this] { showSettingsPanel(); };
        settingsButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        settingsButton.setLookAndFeel (&tactileButtonLookAndFeel());
        settingsButton.setTooltip ("Settings");
        addAndMakeVisible (settingsButton);

        // UI_SPEC_PERFORM.md §2.1: master volume, relocated from the
        // transport row (PX-001's placement) into the header -- a second
        // view onto the SAME mixer.getMasterGain()/setMasterGain() the
        // Master mixer strip already uses, unchanged from PX-001 otherwise.
        outputVolumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        outputVolumeSlider.setRange (0.0, 1.5, 0.01);
        // Phase 1.1 P1 "Volume Display: replace '1.00' with '100%' throughout
        // the UI" -- this slider's own built-in JUCE textbox was the one
        // remaining raw-gain-float readout; MixerChannelStrip's gain label
        // (this file's own custom drawText, above) already shows "100%"
        // style. textFromValueFunction/valueFromTextFunction only affect the
        // textbox's STRING; the slider's internal value/range/step (and
        // mixer.setMasterGain()'s own 0.0-1.5 gain multiplier contract) are
        // completely unchanged -- typing "80%" back in still sets gain 0.8.
        // MUST be set before the first setValue()/setTextBoxStyle() below --
        // Slider::setValue() no-ops (skips updating the textbox entirely,
        // even with dontSendNotification) when the new value equals the
        // current one, so assigning these functions AFTER the initial
        // setValue(1.0) could never actually repaint "1.00" into "100%".
        outputVolumeSlider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
        outputVolumeSlider.valueFromTextFunction  = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue() / 100.0; };
        outputVolumeSlider.setValue (1.0, juce::dontSendNotification);
        outputVolumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
        // Owner: "that part of the app feels foreign" -- explicit palette
        // colours instead of stock-scheme ones: indigo fill on a dim track,
        // and the "100%" box as a borderless card chip rather than an
        // outlined white text field.
        outputVolumeSlider.setColour (juce::Slider::backgroundColourId,        juce::Colour (performlive::kIndigoDim));
        outputVolumeSlider.setColour (juce::Slider::trackColourId,             juce::Colour (performlive::kIndigo));
        outputVolumeSlider.setColour (juce::Slider::thumbColourId,             juce::Colour (performlive::kIndigo).brighter (0.25f));
        outputVolumeSlider.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (performlive::kTextBright));
        outputVolumeSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (performlive::kCard));
        outputVolumeSlider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        outputVolumeSlider.onValueChange = [this] { mixer.setMasterGain ((float) outputVolumeSlider.getValue()); };
        addAndMakeVisible (outputVolumeSlider);

        // UI_SPEC_PERFORM.md §2.1: project selector. Still no multi-project
        // browser (only one project open at a time) -- but bug report: "there's
        // no saving but i see some untitled project somewhere" (and later,
        // the PerformLive UI/UX notes' own "Support New/Open/Open Recent/
        // Save/Save As/Close Project" list) pointed out this was ONLY ever
        // a static label, autosave/save-on-exit the sole persistence path.
        // Reused as a real action menu instead of inventing new header
        // chrome -- see refreshProjectMenu()'s own comment for the full
        // item layout. Item 1 always shows the current project's own
        // filename; every other item snaps back to showing item 1
        // afterward -- this is a menu of actions, not a persistent selection.
        projectSelector.setColour (juce::ComboBox::backgroundColourId, juce::Colour (performlive::kCard));
        projectSelector.setColour (juce::ComboBox::textColourId, juce::Colour (performlive::kTextDim));
        // Owner: "that part of the app feels foreign" -- same tactile
        // gradient/bevel combo face the mixer's output-route box already
        // uses, so the header selector matches the rest of the chrome.
        projectSelector.setLookAndFeel (&tactileButtonLookAndFeel());
        projectSelector.onChange = [this]
        {
            const int id = projectSelector.getSelectedId();
            if (id == kMenuIdSave)        userTriggeredSave();
            else if (id == kMenuIdSaveAs) saveProjectAs();
            else if (id == kMenuIdNew)    newProject();
            else if (id == kMenuIdNewDefault) newProjectFromDefaultPack();
            else if (id == kMenuIdOpen)   openProjectDialog();
            else if (id == kMenuIdClose)  closeProject();
            else if (id >= kMenuIdRecentBase)
            {
                auto recent = recentProjectPaths();
                const int idx = id - kMenuIdRecentBase;
                if (idx >= 0 && idx < recent.size()) openProjectFile (juce::File (recent[idx]));
            }
            refreshProjectMenu();
        };
        addAndMakeVisible (projectSelector);

        // UI_SPEC_PERFORM.md §2/§7: PERFORM/MIXER/LIBRARY/STORE are real
        // full-screen view swaps (refreshActiveView() toggles
        // performView/mixerView/libraryView/storeView's visibility).
        // LIBRARY/STORE no longer open the old modal Explorer dialog
        // (a "squeezed sub-panel," which this spec's own text rules out) --
        // UI_SPEC_LIBRARY.md/UI_SPEC_STORE.md later embedded the real
        // views (MySamplesTab/StoreTab) inline instead, and the old modal
        // (ExplorerPanelContent/showExplorerPanel()) was removed, its
        // purpose fulfilled.
        //
        // Bug report: "let's scrap the stem editor ui entirely from the
        // app... doesn't serve any purpose" -- the dedicated STEM EDITOR nav
        // tab/arrangement view is removed here. Deck-level "stem mode"
        // itself (DeckMode::stem, stemBarLength, stemEndBehavior -- a deck
        // that plays a full-length imported file with its own bar-length
        // detection and end-of-file behavior instead of looping) is a
        // completely separate, still-used PERFORM-view concept and is left
        // untouched; likewise the per-layer clip editor (openClipEditor(),
        // ClipEditorContent) double-tap opens is unrelated and stays.
        static const struct { const char* label; std::function<void (SessionComponent*)> onClick; } kNavItems[] = {
            // Owner: "perform, playback, web and store at one side, then some
            // space, then library and mixer, since they open separately." The
            // four full-screen views first; the two docks after a gap (see
            // resized(), kNavDockGroupGap).
            { "PERFORM",     [] (SessionComponent* s) { s->activeNavIndex = 0; s->refreshActiveView(); } },
            // Section playback: the AbleSet-style performance screen. A full
            // view like PERFORM/STORE, not a dock -- it IS the stage screen.
            { "PLAYBACK",    [] (SessionComponent* s) { s->activeNavIndex = kNavPlayback; s->refreshActiveView(); if (s->playbackView) s->playbackView->songChanged(); } },
            { "WEB",         [] (SessionComponent* s) { s->activeNavIndex = kNavBrowser; s->refreshActiveView(); } },
            { "STORE",       [] (SessionComponent* s) { s->activeNavIndex = 4; s->refreshActiveView(); } },
            // Phase 1.1 P2/P3: LIBRARY and MIXER are no longer mutually-
            // exclusive full-screen views -- they're docked panels sharing
            // ONE bottom dock slot (opening one closes the other, same as
            // any single-panel dock), open AT THE SAME TIME as whichever of
            // PERFORM/STORE is active (this is what makes Phase
            // 1.1 P1's Library-to-deck drag/drop actually reachable: PERFORM
            // stays visible above the dock). Each toggles open/closed
            // rather than switching activeNavIndex.
            { "LIBRARY",     [] (SessionComponent* s)
                { s->libraryDockOpen = ! s->libraryDockOpen;
                  if (s->libraryDockOpen) { s->mixerDockOpen = false; s->closeEditorDock(); }
                  s->refreshActiveView(); s->resized(); s->repaint(); } },
            { "MIXER",       [] (SessionComponent* s)
                { s->mixerDockOpen = ! s->mixerDockOpen;
                  if (s->mixerDockOpen) { s->libraryDockOpen = false; s->closeEditorDock(); }
                  s->refreshActiveView(); s->resized(); s->repaint(); } },
        };
        for (auto& item : kNavItems)
        {
            auto* b = new juce::TextButton (item.label);
            auto onClickCopy = item.onClick;
            b->onClick = [this, onClickCopy] { onClickCopy (this); };
            // UI_SPEC_PERFORM.md §2.1: inactive = transparent fill, kTextDim,
            // no border (was kCard fill in PX-001's own earlier pass).
            b->setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            b->setColour (juce::TextButton::textColourOffId, juce::Colour (performlive::kTextDim));
            b->setLookAndFeel (&tactileButtonLookAndFeel());
            addAndMakeVisible (b);
            navButtons.add (b);
        }

        // UI_SPEC_PERFORM.md §2.2: Play is now ONE button that changes its
        // OWN glyph/fill based on state (triangle+kPlay when stopped,
        // square+kDanger when playing) rather than PX-001's two
        // always-present Play/Stop buttons -- still the exact same single
        // existing ActionId::PlayStop toggle underneath, just unconditional
        // now (no isActiveDeckPlaying() gating needed once there's only one
        // button and it always means "toggle").
        playButton.onClick = [this] { actionRegistry.invoke (ezaction::ActionId::PlayStop); };
        playButton.setLookAndFeel (&tactileButtonLookAndFeel());
        playButton.setTooltip ("Play / stop the active row");
        performView.addAndMakeVisible (playButton);

        // UI_SPEC_PERFORM.md §2.2: Prev/Next deck -- existing
        // ActionId::PrevDeck/NextDeck actions (already registered,
        // keyboard-only via Up/Down since Milestone 14) exposed as buttons
        // for the first time.
        prevDeckButton.setButtonText ("<");
        prevDeckButton.onClick = [this] { actionRegistry.invoke (ezaction::ActionId::PrevDeck); };
        prevDeckButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        prevDeckButton.setLookAndFeel (&tactileButtonLookAndFeel());
        prevDeckButton.setTooltip ("Previous deck");
        performView.addAndMakeVisible (prevDeckButton);

        nextDeckButton.setButtonText (">");
        nextDeckButton.onClick = [this] { actionRegistry.invoke (ezaction::ActionId::NextDeck); };
        nextDeckButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        nextDeckButton.setLookAndFeel (&tactileButtonLookAndFeel());
        nextDeckButton.setTooltip ("Next deck");
        performView.addAndMakeVisible (nextDeckButton);

        // UI_SPEC_PERFORM.md §2.2: tap opens numeric entry -- promptSetMasterTempo().
        bpmReadout.setBpm (masterTempo.bpm);
        bpmReadout.onTap = [this] { promptSetMasterTempo(); };
        bpmReadout.setTooltip ("Tap to set the master tempo");
        performView.addAndMakeVisible (bpmReadout);

        // UI_SPEC_PERFORM.md §2.2: Metro off = kCard, on = kPlay (was
        // kIndigo/kIndigoDim in PX-001 -- this spec calls out kPlay
        // specifically since Metro is transport-timing, not a nav-style toggle).
        // SPEC_PERFORM_V2 GROUP F (104): the plain "M" text glyph is now a
        // real vector metronome icon (IconGlyphButton) -- no setButtonText
        // call needed, paintButton() draws the icon itself.
        metronomeButton.onClick = [this] { actionRegistry.invoke (ezaction::ActionId::ToggleMetronome); };
        metronomeButton.setLookAndFeel (&tactileButtonLookAndFeel());
        metronomeButton.setTooltip ("Metronome on / off");
        performView.addAndMakeVisible (metronomeButton);

        // Milestone 7 built Tab1-4; Milestone 10 completes the mixer view --
        // Pads/Fx/Metro strips (now that all three have real audio to route)
        // plus Master, and every strip's own meter. mixerStrips'  indices
        // match ezdeck::MixerChannel's enum order exactly (Tab1..Metro),
        // which is what timerCallback()'s meter refresh loop relies on.
        // UI_SPEC_PERFORM.md §7 (build-order step 7): re-parented into
        // mixerView -- moved off PERFORM entirely, not deleted; same
        // strips, same wiring, just a different container/visibility.
        // Display labels only -- "Deck 1".."Deck 4" for consistency with
        // PERFORM's own DECK 1-4 column headers. The underlying
        // ezdeck::MixerChannel enum (Tab1..Tab4) and every engine identifier
        // are untouched; only these on-screen strings changed.
        static const juce::String kChannelNames[ezdeck::kNumMixerChannels] = {
            "Deck 1", "Deck 2", "Deck 3", "Deck 4",
            "Deck 5", "Deck 6", "Deck 7", "Deck 8",
            "Live 1", "Live 2", "Live 3", "Live 4",
            "Pads", "FX", "Click", "Cues"
        };
        mixerStripViewport.setViewedComponent (&mixerStripHolder, false);
        mixerStripViewport.setScrollBarsShown (false, true);
        mixerStripViewport.setScrollBarThickness (10);
        mixerView.addAndMakeVisible (mixerStripViewport);
        // UI_SPEC_MIXER.md §1: Deck 1-4 get their own deck-column accent (the
        // same cyan/green/amber/violet the deck grid uses); Pads/FX/Metro use
        // kTextDim (neutral) -- "a channel's colour matches its deck column"
        // is the whole point, so only the first 4 get a real accent.
        for (int c = 0; c < ezdeck::kNumMixerChannels; ++c)
        {
            const bool isTab  = c < ezdeck::kNumLayers;
            const bool isLive = c >= ezdeck::kNumLayers && c < ezdeck::kNumLayers + ezdeck::kNumLiveTracks;
            auto accent = isTab ? performlive::columnAccent (c) : isLive ? juce::Colour (0xffff6b8a) : juce::Colour (performlive::kTextDim);
            // Visual-polish pass: Metro (last channel) gets its own amber
            // wash, matching the reference's own distinctly-tinted Metro
            // strip -- every other channel passes the default transparent.
            const bool isMetro = (c == (int) ezdeck::MixerChannel::Metro);
            const bool isCues  = (c == (int) ezdeck::MixerChannel::Cues);
            auto* strip = new MixerChannelStrip (kChannelNames[c], accent, isTab || isLive, true,
                                                  isMetro ? juce::Colour (0xffffc933)
                                                : isCues  ? juce::Colour (0xff00d9ff) : juce::Colours::transparentBlack);
            const auto channel = (ezdeck::MixerChannel) c;
            strip->onGainChanged = [this, channel] (float g) { mixer.setChannelGain (channel, g); };
            strip->onMuteChanged = [this, channel] (bool m) { mixer.setChannelMute (channel, m); refreshColumnHeaders(); };
            strip->onSoloChanged = [this, channel] (bool s) { mixer.setChannelSolo (channel, s); refreshMixerSoloVisuals(); refreshColumnHeaders(); };
            if (c < kNumStrips)
            {
                strip->onFxToggle  = [this, c] { setStripOn (c, ! stripOn[(size_t) c].load (std::memory_order_relaxed)); };
                strip->onStripMenu = [this, c] { showStripMenu (c); };
            }
            if (isLive)
            {
                const int t = c - ezdeck::kNumLayers;
                strip->setSourceText ("Choose source");
                strip->onSourceClicked = [this, t] { showTrackSourceMenu (t); };
            }
            // SPEC_OUTPUT_ROUTING.md: Phase 1.1 P3 built this selector and
            // its persistence with exactly one documented gap -- "this build
            // has one physical output, so audio still plays through Main."
            // That gap is what this task closes: routeIndexToOutputPair()
            // below maps this selector's own existing vocabulary (Main /
            // Output 1-4 / Custom Bus) onto the Mixer's real output pairs,
            // and mixer.setChannelOutputPair() makes the routing live.
            strip->onOutputRouteChanged = [this, channel, strip] (int routeIndex)
            {
                channelOutputRoute[(size_t) channel] = routeIndex;
                mixer.setChannelOutputPair (channel, routeIndexToOutputPair (routeIndex));
                if (routeIndex != 0)
                {
                    const juce::String label = strip->outputRouteLabel (routeIndex);
                    // The engine clamps a pair the device doesn't have back
                    // to Main every block, so the choice is remembered (and
                    // comes alive when a bigger interface is plugged in) but
                    // is silent right now. Say so, rather than let someone
                    // wonder why Out 5/6 on a 4-output box makes no sound.
                    if (routeIndexToOutputPair (routeIndex) >= pairCount())
                        showToast (kChannelNames[(int) channel] + ": \"" + label + "\" isn't on this audio device (it has "
                                   + juce::String (pairCount()) + " pair" + (pairCount() == 1 ? "" : "s")
                                   + ") -- playing through Main until it is");
                    else
                        showToast (kChannelNames[(int) channel] + ": routed to \"" + label + "\" -- direct to hardware, bypasses Master");
                }
            };
            mixerStripHolder.addAndMakeVisible (strip);
            mixerStrips.add (strip);
        }
        {
            // Visual-polish pass: Master gets its own indigo wash, matching
            // the reference's own distinctly-tinted Master strip.
            auto* strip = new MixerChannelStrip ("Master", juce::Colour (performlive::kTextDim), false, false,
                                                  juce::Colour (performlive::kIndigo));
            strip->onGainChanged = [this] (float g) { mixer.setMasterGain (g); };
            mixerView.addAndMakeVisible (strip);
            masterStrip.reset (strip);
        }
        {
            // Owner: "the browser and YouTube should have a dedicated mixer
            // channel." WebView2 plays through Windows, not through this
            // mixer, so the strip sets the page's own players' level and
            // mute (BrowserTab::setMediaLevel) -- no meter, no solo, no
            // routing, because no audio passes through here.
            auto* strip = new MixerChannelStrip ("Web", juce::Colour (performlive::kTextDim), false, true, juce::Colour (0xffff7a45));
            strip->setMeterVisible (false);
            strip->setSoloAvailable (false);
            strip->setRoutable (false);
            strip->onGainChanged = [this] (float g) { webGain = g; pushWebLevel(); };
            strip->onMuteChanged = [this] (bool m) { webMute = m; pushWebLevel(); };
            mixerStripHolder.addAndMakeVisible (strip);
            webStrip.reset (strip);
        }
        refreshMixerSoloVisuals();

        // Milestone 8: pads. Exclusive by default (PRD §8). Real audio is
        // loaded only for whichever of pad1.wav..pad12.wav actually exist on
        // disk (this app's established, hardcoded-file-naming convention,
        // Milestone 6's own precedent) -- every other slot starts empty.
        // Clicking an empty slot is a no-op: this app has no clip-loader UI
        // yet (NEXT_STEPS.md #12), so there is nothing to "open" -- a
        // documented scope boundary, not a silent omission.
        padBank.exclusive = true;
        // Ship-empty: the pad1.wav..pad12.wav fixture autoload is gone --
        // pads start empty; see the deck-autoload removal comment above.
        for (int i = 0; i < 12; ++i)
            padPanel.setSlotBoundKey (i, keyForAction (ezaction::padAction (i)));

        padPanel.onSlotClicked = [this] (int i)
        {
            // Owner #7: "select pads to play and deselect to stop" -- a tap
            // on a SOUNDING pad always stops it (click-free requestStop),
            // regardless of Trigger Mode; a tap on a silent pad plays it.
            // (Previously only loop-mode pads toggled; one-shots retriggered,
            // with no way to stop a long pad sample mid-flight.)
            auto& v = padBank.voices[(size_t) i];
            if (! v.loaded) return;
            if (v.isActive()) padBank.stopVoice (i);
            else padBank.triggerVoice (i);
        };
        // Owner #5: right-click = context MENU (colour swatches on top,
        // MIDI Learn included); double-click = the editor.
        padPanel.onSlotRightClicked   = [this] (int i) { showVoiceSlotContextMenu (true, i); };
        padPanel.onSlotDoubleClicked  = [this] (int i) { openVoiceEditor (true, i); };
        // Phase 1.1 P6: pages are real now (see VoiceBankPanel's own header
        // comment) -- no toast needed, onPageSelected has nothing to do here.
        // Owner request: pads accept drops -- a Library card straight into
        // this slot, or OS files (load in place, then offer a Library add).
        padPanel.onSlotAssetDropped = [this] (int i, const juce::String& assetId) { loadAssetIntoVoiceSlot (true, i, assetId); };
        padPanel.onSlotFilesDropped = [this] (int i, const juce::StringArray& files) { loadDroppedFilesIntoVoiceSlot (true, i, files); };
        // SPEC_PERFORM_V2 GROUP H2: "Pads won't load -- fix it." Tapping an
        // empty pad previously did nothing at all (onSlotClicked's own
        // `if (loaded)` guard silently no-ops otherwise); the "..." glyph is
        // new on every slot. Both arm the SAME pending-load mechanism
        // Group B's deck slots already use, just for a voice slot.
        padPanel.onSlotLibraryTap = [this] (int i)
        {
            pendingLoadVoiceIndex = i;
            pendingLoadIsPad = true;
            libraryDockOpen = true;
            mixerDockOpen = false;
            closeEditorDock();
            refreshActiveView();
            resized();
            showToast ("Pad " + juce::String (i + 1) + ": tap a Library sample to load it here");
        };
        performView.addAndMakeVisible (padPanel);

        // Milestone 9: FX -- the identical VoiceBank<12> type, non-exclusive
        // (PRD §9 states no such constraint, unlike Pads). "One-shots fire
        // and end; loops toggle" (PRD §9): a non-looping slot always
        // retriggers on click (fire-and-forget, never needs an explicit
        // stop); a looping slot's click toggles it off if already playing.
        // No loop-toggle UI exists yet (no clip editor for FX) -- every
        // loaded FX clip defaults to loop=false (one-shot) until that UI
        // exists; the engine-level loop behavior itself is fully built and
        // tested (voicetest.cpp already covers it generically).
        fxBank.exclusive = false;
        // Ship-empty: the fx1.wav..fx12.wav fixture autoload is gone -- FX
        // slots start empty; see the deck-autoload removal comment above.
        for (int i = 0; i < 12; ++i)
            fxPanel.setSlotBoundKey (i, keyForAction (ezaction::fxAction (i)));

        fxPanel.onSlotClicked = [this] (int i)
        {
            auto& v = fxBank.voices[(size_t) i];
            if (! v.loaded) return;
            if (v.loop && v.isActive()) fxBank.stopVoice (i);
            else fxBank.triggerVoice (i);
        };
        fxPanel.onSlotRightClicked  = [this] (int i) { showVoiceSlotContextMenu (false, i); };
        fxPanel.onSlotDoubleClicked = [this] (int i) { openVoiceEditor (false, i); };
        // FX equivalents of padPanel's own drop wiring just above.
        fxPanel.onSlotAssetDropped = [this] (int i, const juce::String& assetId) { loadAssetIntoVoiceSlot (false, i, assetId); };
        fxPanel.onSlotFilesDropped = [this] (int i, const juce::StringArray& files) { loadDroppedFilesIntoVoiceSlot (false, i, files); };
        // SPEC_PERFORM_V2 GROUP H2: FX equivalent of padPanel's own
        // onSlotLibraryTap just above.
        fxPanel.onSlotLibraryTap = [this] (int i)
        {
            pendingLoadVoiceIndex = i;
            pendingLoadIsPad = false;
            libraryDockOpen = true;
            mixerDockOpen = false;
            closeEditorDock();
            refreshActiveView();
            resized();
            showToast ("FX " + juce::String (i + 1) + ": tap a Library sample to load it here");
        };
        performView.addAndMakeVisible (fxPanel);

        // Milestone 11: the scene bar -- 8 slots, hold-to-save/tap-to-recall
        // (PRD §11/§16). All start empty; nothing to load from disk (scenes
        // are pure structural state, not audio).
        for (int i = 0; i < 8; ++i)
        {
            auto* b = new SceneButton (i + 1);
            b->onTap          = [this, i] { recallScene (i); };
            b->onHoldComplete = [this, i] { saveScene (i); };
            b->onRightClick   = [this, i] { showSceneMenu (i); };
            b->setTooltip (juce::String (juce::CharPointer_UTF8 ("Tap to recall " "\xc2" "\xb7" " hold to save " "\xc2" "\xb7" " right-click to recolour")));
            performView.addAndMakeVisible (b);
            sceneButtons.add (b);
        }

        // UI_SPEC_PERFORM.md §7: the four view containers themselves.
        // libraryView/storeView are explicit placeholders this step ("can be
        // placeholder full-screen containers for now") -- mixerView holds
        // the real, just-relocated strips above; performView holds
        // everything else already re-parented above. toast stays a direct
        // child of SessionComponent, not any view -- notifications should
        // work no matter which view is active.
        // PERFORM scroll (owner bug report -- see PerformContentView's own
        // comment): performView is hosted in a viewport, vertical-only, so
        // a dock can never make pads/FX unreachable.
        performScroll.setViewedComponent (&performView, false);
        performScroll.setScrollBarsShown (true, false);
        performScroll.setScrollBarThickness (10);
        addAndMakeVisible (performScroll);
        addAndMakeVisible (mixerView);
        addAndMakeVisible (libraryView);
        addAndMakeVisible (editorView);
        addAndMakeVisible (storeView);

        // UI_SPEC_LIBRARY.md: the real LIBRARY view -- replaces the old
        // placeholder label. MySamplesTab is the exact class
        // showExplorerPanel()'s own comment named as "exactly this call
        // away from being embedded inline once LIBRARY/STORE get built out
        // past their current placeholder" -- this is that call.
        libraryTab = std::make_unique<MySamplesTab> (
            libraryManager, formatManager,
            [this] (juce::String message) { showToast (message); },
            [this] (std::shared_ptr<const std::vector<float>> left, std::shared_ptr<const std::vector<float>> right, double fileSampleRate)
            {
                // file rate -> playback ratio here, where the device rate is
                // known (pad pitch fix, applied to the audition path too)
                startLibraryPreview (std::move (left), std::move (right),
                                     fileSampleRate > 0.0 && currentSampleRate > 0.0
                                       ? fileSampleRate / currentSampleRate : 1.0);
            },
            [this] { stopLibraryPreview(); },
            [this] { return isLibraryPreviewActive(); });
        libraryView.addAndMakeVisible (*libraryTab);

        // SPEC_PERFORM_V2 GROUP B: consumes the pending-load target an empty
        // deck's tap armed (see the deck-grid card->onTap above), loading
        // this tapped Library card into that slot instead of auditioning
        // it. Returns false (so the tab falls through to its normal
        // audition behavior) whenever no target is pending.
        libraryTab->onCardTapMaybeLoad = [this] (const juce::String& assetId) -> bool
        {
            if (pendingLoadDeck >= 0)
            {
                const int d = pendingLoadDeck, l = pendingLoadLayer;
                pendingLoadDeck = -1;
                pendingLoadLayer = -1;
                loadAssetIntoDeckSlot (d, l, assetId);
                return true;
            }
            // SPEC_PERFORM_V2 GROUP H2: same pending-load mechanism, for a
            // Pad/FX slot instead of a deck layer -- see padPanel/fxPanel's
            // own onSlotLibraryTap wiring below for where this gets armed.
            if (pendingLoadVoiceIndex >= 0)
            {
                const bool isPad = pendingLoadIsPad;
                const int idx = pendingLoadVoiceIndex;
                pendingLoadVoiceIndex = -1;
                loadAssetIntoVoiceSlot (isPad, idx, assetId);
                return true;
            }
            return false;
        };

        // Phase 1.1 P2: the dock's own chrome -- drag-to-resize header strip
        // plus Maximize/Restore and Close. See DockHeaderBar's own comment
        // for why this is written generically (Priority 3's Mixer dock
        // reuses it, not a copy).
        libraryDockHeader.setTitle ("LIBRARY");
        libraryDockHeader.onDrag = [this] (int dy)
        {
            const int totalH = juce::jmax (1, getHeight() - kHeaderHeight);
            libraryDockHeightFraction = juce::jlimit (0.2f, 0.9f,
                libraryDockHeightFraction - (float) dy / (float) totalH);
            resized();
            repaint();   // see the Maximize handler's own comment below on why resized() alone can leave stale pixels
        };
        libraryView.addAndMakeVisible (libraryDockHeader);

        libraryMaximizeButton.setButtonText ("Maximize");
        libraryMaximizeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        libraryMaximizeButton.setLookAndFeel (&tactileButtonLookAndFeel());   // owner: match the app's tactile chrome
        libraryMaximizeButton.onClick = [this]
        {
            // libraryDockHeightFraction itself is untouched here -- resized()
            // now handles maximized as "take the whole below-header area,"
            // not as a bigger fraction (see its own comment for why).
            // libraryDockMaximized alone is what it reads.
            libraryDockMaximized = ! libraryDockMaximized;
            libraryMaximizeButton.setButtonText (libraryDockMaximized ? "Restore" : "Maximize");
            resized();
            // Caught during this session's own screenshot verification: a
            // real bug, not theoretical. resized() alone changes bounds but
            // doesn't reliably force JUCE to repaint the region performView
            // (or whichever main view) previously occupied and this dock
            // has now grown into -- toFront() only fixes ongoing z-order,
            // it doesn't invalidate already-painted pixels from before the
            // resize. Without this, maximizing left stale PERFORM content
            // visible through the gaps between mixer strips/library cards.
            repaint();
        };
        libraryView.addAndMakeVisible (libraryMaximizeButton);

        libraryCloseButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));   // U+00D7 MULTIPLICATION SIGN, i.e. "x"
        libraryCloseButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        libraryCloseButton.setLookAndFeel (&tactileButtonLookAndFeel());
        libraryCloseButton.onClick = [this] { libraryDockOpen = false; refreshActiveView(); resized(); };
        libraryView.addAndMakeVisible (libraryCloseButton);

        // Phase 1.1 P3 "Mixer: Open docked, allow maximize, like Library" --
        // identical wiring to the Library dock just above, reusing the same
        // DockHeaderBar class.
        mixerDockHeader.setTitle ("MIXER");
        mixerDockHeader.onDrag = [this] (int dy)
        {
            const int totalH = juce::jmax (1, getHeight() - kHeaderHeight);
            mixerDockHeightFraction = juce::jlimit (0.2f, 0.9f,
                mixerDockHeightFraction - (float) dy / (float) totalH);
            resized();
            repaint();   // see the Maximize handler's own comment below on why resized() alone can leave stale pixels
        };
        mixerView.addAndMakeVisible (mixerDockHeader);

        mixerMaximizeButton.setButtonText ("Maximize");
        mixerMaximizeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        mixerMaximizeButton.setLookAndFeel (&tactileButtonLookAndFeel());
        mixerMaximizeButton.onClick = [this]
        {
            mixerDockMaximized = ! mixerDockMaximized;
            mixerMaximizeButton.setButtonText (mixerDockMaximized ? "Restore" : "Maximize");
            resized();
            repaint();   // real bug caught live -- see libraryMaximizeButton's own onClick comment above
        };
        mixerView.addAndMakeVisible (mixerMaximizeButton);

        mixerCloseButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
        mixerCloseButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        mixerCloseButton.setLookAndFeel (&tactileButtonLookAndFeel());
        mixerCloseButton.onClick = [this] { mixerDockOpen = false; refreshActiveView(); resized(); };
        mixerView.addAndMakeVisible (mixerCloseButton);

        // PerformLive UI/UX Design Notes: the third dock slot -- Pad/FX/Deck
        // editing "reuses the exact same editor/styling/workflow," so this
        // is the identical DockHeaderBar + Maximize/Close chrome as Library/
        // Mixer above, just hosting whichever ClipEditorContent is currently
        // open (see openClipEditor()/closeEditorDock()) instead of a fixed
        // child. No title is set here -- openClipEditor() sets it per-clip.
        editorDockHeader.onDrag = [this] (int dy)
        {
            const int totalH = juce::jmax (1, getHeight() - kHeaderHeight);
            editorDockHeightFraction = juce::jlimit (0.2f, 0.9f,
                editorDockHeightFraction - (float) dy / (float) totalH);
            resized();
            repaint();
        };
        editorView.addAndMakeVisible (editorDockHeader);

        editorMaximizeButton.setButtonText ("Maximize");
        editorMaximizeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        editorMaximizeButton.setLookAndFeel (&tactileButtonLookAndFeel());
        editorMaximizeButton.onClick = [this]
        {
            editorDockMaximized = ! editorDockMaximized;
            editorMaximizeButton.setButtonText (editorDockMaximized ? "Restore" : "Maximize");
            resized();
            repaint();
        };
        editorView.addAndMakeVisible (editorMaximizeButton);

        editorCloseButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
        editorCloseButton.setColour (juce::TextButton::buttonColourId, juce::Colour (performlive::kCard));
        editorCloseButton.setLookAndFeel (&tactileButtonLookAndFeel());
        editorCloseButton.onClick = [this] { closeEditorDock(); resized(); };
        editorView.addAndMakeVisible (editorCloseButton);

        // Owner: "build me a fake store that looks like this ... all buttons
        // say coming soon." STORE opens on the showcase (StoreShowcase.h), with
        // the creator sign-up at the top. Its "Creator details" opens the
        // creators page (logo, email, Instagram, the WhatsApp group, the beta's
        // end date); the back button returns to the showcase. The online pack
        // store is not in this build.
        creatorsTab = std::make_unique<creators::CreatorsTab> (juce::JUCEApplication::getInstance() != nullptr
                                                                   ? juce::JUCEApplication::getInstance()->getApplicationVersion() : juce::String());
        storeView.addAndMakeVisible (*creatorsTab);
        storeShowcase = std::make_unique<ezstore::StoreShowcase>();
        storeShowcase->onComingSoon   = [this] (const juce::String& what) { showToast (what + " -- coming soon"); };
        storeShowcase->onOpenCreators = [this] { showStorePage (StorePage::creators); };
        storeView.addAndMakeVisible (*storeShowcase);
        storeBackButton.onClick = [this] { showStorePage (StorePage::showcase); };
        storeView.addChildComponent (storeBackButton);
        showStorePage (StorePage::showcase);

        // PLAYBACK: the performance screen. It owns no state -- see
        // PlaybackView.h -- everything comes through the PlaybackHost
        // methods this class implements further down.
        addChildComponent (playbackHolder);
        playbackView = std::make_unique<ezplayback::PlaybackView> (*this);
        playbackHolder.addAndMakeVisible (*playbackView);

        refreshActiveView();   // establishes the initial PERFORM-visible/others-hidden state

        addAndMakeVisible (toast);

        // Milestone 12: a saved project, if one exists, overrides the
        // hardcoded defaults just loaded above -- purely additive; with no
        // project file present, behavior is completely unchanged from every
        // prior milestone (backward compatible by construction, not by a
        // special-cased branch).
        currentProjectFile = computeDefaultProjectFilePath();
        loadProjectFile();
        refreshProjectMenu();

        // roadmap "Sprint 5: reliability" -- deferred so the recovery
        // prompt (an AlertWindow) never appears before the main window
        // itself is actually up; see offerCrashRecoveryIfNeeded()'s own
        // comment for the detection logic. SafePointer (not a raw `this`
        // capture) guards the -- admittedly unlikely -- case of the window
        // closing again before this next message-loop iteration runs.
        {
            juce::Component::SafePointer<SessionComponent> safeThis (this);
            juce::MessageManager::callAsync ([safeThis]
            {
                if (auto* self = safeThis.getComponent()) self->offerCrashRecoveryIfNeeded();
            });
        }

        // Milestone 14: registers every PRD §15 action's real behavior ONCE,
        // reused identically by keyboard and MIDI (and, where one already
        // existed, by the same mouse-click handler) -- never a second,
        // parallel implementation for the same action.
        registerActions();

        setWantsKeyboardFocus (true);

        // Milestone 14: opens every available MIDI input device. Marshaled
        // to the message thread inside MidiActionRouter itself -- see that
        // class's own header comment.
        midiRouter = std::make_unique<ezaction::MidiActionRouter> (actionRegistry);
        // PX-D: notes reach the live tracks' instruments straight from the MIDI
        // thread -- each track listens to every channel, or just its own.
        midiRouter->onRawMessage = [this] (const juce::MidiMessage& m)
        {
            for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
            {
                auto& s = instruments[(size_t) t];
                if (! s.hasInstrument()) continue;
                const int want = trackMidiChannel[(size_t) t].load (std::memory_order_relaxed);
                if (want == 0 || m.getChannel() == 0 || m.getChannel() == want) s.addMidi (m);
            }
        };
        for (auto& device : juce::MidiInput::getAvailableDevices())
        {
            if (auto midiIn = juce::MidiInput::openDevice (device.identifier, midiRouter.get()))
            {
                midiIn->start();
                openMidiDeviceNames.add (device.name);
                openMidiInputs.push_back (std::move (midiIn));
            }
        }

        // UI_SPEC_PERFORM.md steps 4/6: width kept at PX-001/step-7's 1310
        // (rail shrinking 170->88 just gives the deck grid extra width, no
        // reason to shrink the window for it). Height grows by exactly
        // kStatusBarHeight (26, new this step) -- top chrome SHRINKS
        // (136->120: header+nav merge from 64+40 to one 56px row, scenes
        // move out of their own 32px row into the 64px transport+scenes
        // row), and the old bottom transport strip is gone entirely (moved
        // up into that same row), so the deck grid panel ends up with
        // considerably MORE height than before -- exactly the "let the deck
        // grid absorb the reclaimed blank strip" step 4 asks for.
        setSize (1310, 902);

        // Open the audio device the owner chose LAST time -- driver type
        // (ASIO / Windows Audio), device, sample rate, buffer size -- rather
        // than whatever Windows considers the default. A performer who picked
        // "UMC ASIO Driver" in Settings once should not have to pick it again
        // at every launch; that is table stakes for a DAW, and until this the
        // app forgot the choice on exit. The XML is JUCE's own device-state
        // format, saved by saveAudioDeviceState() whenever the device changes.
        // If the saved device is unplugged today, JUCE falls back to a working
        // default rather than failing -- so this can never stop the app opening.
        std::unique_ptr<juce::XmlElement> savedAudioState;
        if (auto* settings = getAppSettings())
            savedAudioState = settings->getXmlValue ("audioDeviceState");
        setAudioChannels (0, 2, savedAudioState.get());   // 0 in, >= 2 out; requestAllOutputChannels() widens it below

        requestAllOutputChannels();
        saveAudioDeviceState();   // capture whatever we ended up with, so a fresh install remembers its first device too

        // ...and again whenever the audio device changes. Doing this only in
        // the constructor was a real gap: it worked if the interface was
        // already plugged in at launch, and silently didn't if you connected
        // it afterwards or picked it in Settings > Audio -- JUCE reopens a
        // newly selected device with its DEFAULT (stereo) channel set, so
        // Out 3/4 and beyond simply never appeared. Plugging a sound card
        // into a running app is the normal case on stage, not the exception.
        deviceManager.addChangeListener (this);

        startTimerHz (15);         // UI refresh only -- audio is never driven by this
        grabKeyboardFocus();
    }

    /**
     * Asks the open device for EVERY output channel it has, rather than the
     * stereo pair JUCE hands out by default. Without this, a channel routed to
     * Out 3/4 has no physical output to reach no matter what the mixer says.
     *
     * Safe to call repeatedly. It does nothing when the device is already
     * fully open, which also breaks the feedback loop: setAudioDeviceSetup()
     * itself fires the change notification that brought us here.
     */
    /** PX-C: opens every input the device has once any column asks for a
        live input (and leaves them closed otherwise -- an open input the app
        never reads is just latency and CPU). */
    void requestLiveInputChannels()
    {
        if (reconfiguringInputs) return;
        auto* device = deviceManager.getCurrentAudioDevice();
        if (device == nullptr) return;

        bool wanted = false;
        for (auto& c : trackInput) if (c.load (std::memory_order_relaxed) >= 0) wanted = true;

        const int available = device->getInputChannelNames().size();
        const int open      = device->getActiveInputChannels().countNumberOfSetBits();
        if (! wanted || available <= 0 || open >= available) return;

        const juce::ScopedValueSetter<bool> guard (reconfiguringInputs, true);
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.inputChannels.setRange (0, available, true);
        const auto error = deviceManager.setAudioDeviceSetup (setup, true);
        if (error.isNotEmpty())
            juce::Logger::writeToLog ("Could not open inputs on " + device->getName() + ": " + error);
        else if (auto* now = deviceManager.getCurrentAudioDevice())
            juce::Logger::writeToLog ("Live input: opened " + juce::String (now->getActiveInputChannels().countNumberOfSetBits())
                                      + " of " + juce::String (available) + " inputs on " + now->getName());
    }

    /** Names the device's inputs for menus: "In 1", "In 2 (Mic/Line 2)". */
    juce::StringArray liveInputNames() const
    {
        juce::StringArray out;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            for (const auto& n : device->getInputChannelNames()) out.add (n);
        return out;
    }

    //== live tracks (LIVE 1-4 on the mixer) =======================================
    // Owner: "separate tracks for real instruments and microphone inputs,
    // apart from the decks, so the eight decks stay for stems -- not on the
    // perform view, set up in the mixer." A live track is a mic / DI input or
    // an instrument plugin, on its own mixer channel (MixerChannel::Live1+t).

    static juce::String trackLabel (int t) { return "LIVE " + juce::String (t + 1); }

    void setTrackInput (int t, int channel, bool stereo)
    {
        if (t < 0 || t >= ezdeck::kNumLiveTracks) return;
        if (channel >= 0 && instruments[(size_t) t].hasInstrument())
            setTrackInstrument (t, std::nullopt);   // a track is one thing: an input or an instrument
        trackInput[(size_t) t].store (channel, std::memory_order_relaxed);
        trackStereo[(size_t) t].store (stereo, std::memory_order_relaxed);
        requestLiveInputChannels();
        if (channel >= 0) ensureChannelAudible (ezdeck::kNumLayers + t);
        refreshTrackSourceLabels();
        if (channel >= 0)
            showToast (trackLabel (t) + ": "
                       + (stereo ? "In " + juce::String (channel + 1) + "+" + juce::String (channel + 2)
                                 : "In " + juce::String (channel + 1))
                       + " -- level, mute, FX and output on its mixer strip");
    }

    /** "Mic  In 3", "EZkeys 2", "Choose source" -- the live strip's source button. */
    juce::String trackSourceLabel (int t) const
    {
        if (instruments[(size_t) t].hasInstrument())
        {
            const int midi = trackMidiChannel[(size_t) t].load (std::memory_order_relaxed);
            return instruments[(size_t) t].currentDescription().name + (midi > 0 ? "  ch " + juce::String (midi) : juce::String());
        }
        const int ch = trackInput[(size_t) t].load (std::memory_order_relaxed);
        if (ch < 0) return "Choose source";
        return trackStereo[(size_t) t].load (std::memory_order_relaxed)
             ? "In " + juce::String (ch + 1) + "+" + juce::String (ch + 2)
             : "Mic  In " + juce::String (ch + 1);
    }

    void refreshTrackSourceLabels()
    {
        for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
            if (ezdeck::kNumLayers + t < mixerStrips.size())
                mixerStrips[ezdeck::kNumLayers + t]->setSourceText (trackSourceLabel (t));
    }

    void showTrackSourceMenu (int t)
    {
        juce::PopupMenu m;
        m.addSectionHeader (trackLabel (t) + "  -  SOURCE");
        if (instruments[(size_t) t].hasInstrument())
        {
            m.addItem (8001, "Open " + instruments[(size_t) t].currentDescription().name + "...");
            m.addItem (8002, "All notes off");
            m.addSeparator();
        }
        addTrackInputMenu (m, t);
        addInstrumentMenu (m, t);
        {
            juce::PopupMenu midi;
            const int want = trackMidiChannel[(size_t) t].load (std::memory_order_relaxed);
            midi.addItem (9500, "Every channel", true, want == 0);
            midi.addSeparator();
            for (int ch = 1; ch <= 16; ++ch) midi.addItem (9500 + ch, "Channel " + juce::String (ch), true, want == ch);
            m.addSubMenu ("MIDI channel", midi, instruments[(size_t) t].hasInstrument());
        }
        m.addSeparator();
        m.addItem (9990, "No source", instruments[(size_t) t].hasInstrument() || trackInput[(size_t) t].load (std::memory_order_relaxed) >= 0);

        auto options = juce::PopupMenu::Options();
        if (ezdeck::kNumLayers + t < mixerStrips.size()) options = options.withTargetComponent (mixerStrips[ezdeck::kNumLayers + t]);
        m.showMenuAsync (options, [this, t] (int r)
        {
            if (r <= 0) return;
            if (r == 9990) { setTrackInstrument (t, std::nullopt); setTrackInput (t, -1, false); refreshTrackSourceLabels(); return; }
            if (r >= 9500 && r <= 9516)
            {
                trackMidiChannel[(size_t) t].store (r - 9500, std::memory_order_relaxed);
                refreshTrackSourceLabels();
                showToast (trackLabel (t) + ": MIDI " + (r == 9500 ? juce::String ("every channel") : "channel " + juce::String (r - 9500)));
                return;
            }
            if (handleTrackInputMenu (t, r)) return;
            handleInstrumentMenu (t, r);
        });
    }

    // Owner: "the mic input receives signal, I see it in the channel strip,
    // but I don't hear any sound and the mixer shows nothing." The column's
    // mixer channel was muted (the strip sits before the mixer, so its meter
    // still moved). Putting a mic or an instrument on a column is asking to
    // hear it, so a mute on that channel is lifted and said out loud.
    void ensureChannelAudible (int channel)
    {
        if (channel < 0 || channel >= ezdeck::kNumMixerChannels) return;
        const auto ch = (ezdeck::MixerChannel) channel;
        if (mixer.getChannelMute (ch))
        {
            mixer.setChannelMute (ch, false);
            if (channel < mixerStrips.size()) mixerStrips[channel]->setMuteState (false);
            showToast (stripLabel (channel) + ": its mixer channel was muted -- unmuted so you can hear it");
        }
    }

    /** DECK 1-8, LIVE 1-4 -- one name per channel strip / mixer channel. */
    static juce::String stripLabel (int i)
    {
        return i < ezdeck::kNumLayers ? "DECK " + juce::String (i + 1) : trackLabel (i - ezdeck::kNumLayers);
    }

    /** "MUTED" for the deck header when the column's mixer channel can't be heard. */
    bool columnSilenced (int layer) const
    {
        const auto ch = (ezdeck::MixerChannel) layer;
        if (mixer.getChannelMute (ch)) return true;
        bool anySolo = false;
        for (int c = 0; c < ezdeck::kNumMixerChannels; ++c) if (mixer.getChannelSolo ((ezdeck::MixerChannel) c)) anySolo = true;
        return anySolo && ! mixer.getChannelSolo (ch) && ! mixer.isExemptFromSolo (ch);
    }

    void refreshColumnHeaders() { setDeckPage (deckPage); }   // the header text is built in setDeckPage()

    /** The "Mic / DI input" part of a live track's source menu. */
    void addTrackInputMenu (juce::PopupMenu& into, int t)
    {
        juce::PopupMenu m;
        const auto names = liveInputNames();
        const int current = trackInput[(size_t) t].load (std::memory_order_relaxed);
        const bool stereo = trackStereo[(size_t) t].load (std::memory_order_relaxed);
        m.addItem (9000, "None", true, current < 0);
        if (names.isEmpty())
        {
            m.addItem (9001, "This audio device has no inputs -- choose an interface in Settings > Audio", false);
        }
        else
        {
            m.addSeparator();
            for (int i = 0; i < names.size(); ++i)
                m.addItem (9100 + i, "In " + juce::String (i + 1) + "  " + names[i], true, current == i && ! stereo);
            if (names.size() >= 2)
            {
                m.addSeparator();
                for (int i = 0; i + 1 < names.size(); i += 2)
                    m.addItem (9300 + i, "In " + juce::String (i + 1) + "+" + juce::String (i + 2) + "  stereo", true, current == i && stereo);
            }
        }
        into.addSubMenu ("Mic / DI input", m);
    }

    // ---- PX-D: instruments (on the live tracks) -----------------------------------

    void addInstrumentMenu (juce::PopupMenu& into, int t)
    {
        juce::PopupMenu m;
        auto& slot = instruments[(size_t) t];
        const auto list = pluginLibrary.instruments();
        m.addItem (8000, "None", true, ! slot.hasInstrument());
        if (slot.hasInstrument())
        {
            m.addItem (8001, "Open " + slot.currentDescription().name + "...");
            m.addItem (8002, "All notes off");
        }
        m.addSeparator();
        if (list.isEmpty())
            m.addItem (8003, "No instruments found yet -- scan for plugins");
        else
            for (int i = 0; i < list.size() && i < 500; ++i)
                m.addItem (8100 + i, list[i].name + "  (" + list[i].pluginFormatName + ")", true,
                           slot.hasInstrument() && slot.currentDescription().createIdentifierString() == list[i].createIdentifierString());
        m.addSeparator();
        m.addItem (8003, "Scan for plugins...");
        m.addItem (8004, "Plugin list...");
        into.addSubMenu ("Instrument (VST3)", m);
    }

    bool handleInstrumentMenu (int t, int result)
    {
        if (result == 8000) { setTrackInstrument (t, std::nullopt); return true; }
        if (result == 8001) { openInstrumentEditor (t); return true; }
        if (result == 8002) { instruments[(size_t) t].allNotesOff(); return true; }
        if (result == 8003) { startPluginScan(); return true; }
        if (result == 8004) { showPluginScanner(); return true; }
        if (result >= 8100 && result < 8600)
        {
            const auto list = pluginLibrary.instruments();
            if (result - 8100 < list.size()) setTrackInstrument (t, list[result - 8100]);
            return true;
        }
        return false;
    }

    /** Loads (or clears) a live track's instrument. Message thread; the plugin
        is created and prepared before the audio thread ever sees it. */
    void setTrackInstrument (int t, std::optional<juce::PluginDescription> desc, const juce::String& state = {})
    {
        if (t < 0 || t >= ezdeck::kNumLiveTracks) return;
        if (desc.has_value() && ! desc->isInstrument)
        {
            showToast (desc->name + " is an effect. Live tracks take instrument plugins only -- every track has the built-in PERFORM LIVE channel strip (FX) for effects.");
            return;
        }
        instrumentWindows[(size_t) t] = nullptr;
        auto& slot = instruments[(size_t) t];
        if (! desc.has_value())
        {
            slot.clear();
            refreshTrackSourceLabels();
            return;
        }
        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        // A plugin that throws while loading must not take the show down.
        try
        {
            instance = pluginLibrary.formatManager().createPluginInstance (*desc, currentSampleRate > 0.0 ? currentSampleRate : 44100.0,
                                                                            juce::jmax (64, mixerScratchCapacity), error);
            if (instance == nullptr)
            {
                showToast ("Couldn't load " + desc->name + ": " + error);
                juce::Logger::writeToLog ("Instrument load failed: " + desc->name + " -- " + error);
                return;
            }
            slot.install (std::move (instance), *desc);
            slot.setStateFromString (state);
        }
        catch (const std::exception& e)
        {
            juce::Logger::writeToLog ("Instrument threw while loading: " + desc->name + " -- " + juce::String (e.what()));
            showToast (desc->name + " failed while loading (" + juce::String (e.what()) + ") -- not added");
            return;
        }
        catch (...)
        {
            juce::Logger::writeToLog ("Instrument threw while loading: " + desc->name);
            showToast (desc->name + " failed while loading -- not added");
            return;
        }
        if (trackInput[(size_t) t].load (std::memory_order_relaxed) >= 0)
        {
            trackInput[(size_t) t].store (-1, std::memory_order_relaxed);   // a track is one thing: instrument or input
            trackStereo[(size_t) t].store (false, std::memory_order_relaxed);
        }
        ensureChannelAudible (ezdeck::kNumLayers + t);
        refreshTrackSourceLabels();
        if (state.isEmpty())
        {
            showToast (trackLabel (t) + ": " + desc->name + " -- play it from your MIDI keyboard; its source button opens it again");
            openInstrumentEditor (t);   // the first thing anyone wants after loading an instrument is its window
        }
    }

    void openInstrumentEditor (int t)
    {
        auto& slot = instruments[(size_t) t];
        auto* instance = slot.instanceForEditor();
        if (instance == nullptr) return;
        auto& win = instrumentWindows[(size_t) t];
        if (win != nullptr) { win->toFront (true); return; }
        win = std::make_unique<ezinst::InstrumentEditorWindow> (*instance, slot.currentDescription().name + "  --  " + trackLabel (t),
                                                                 [this, t] { instrumentWindows[(size_t) t] = nullptr; });
    }

    /** Scans the standard VST3 folders straight away, with a progress window
        and a Cancel button -- no folder dialog. Each file is probed in the
        separate scanner process (InstrumentHost.h). */
    void startPluginScan()
    {
        if (pluginScan != nullptr) return;

        class Scan final : public juce::ThreadWithProgressWindow
        {
        public:
            explicit Scan (SessionComponent& o)
                : juce::ThreadWithProgressWindow ("Scanning for instruments...", true, true, 10000, "Cancel"), owner (o) {}

            void run() override
            {
                const auto pedal = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                       .getChildFile ("EzPlay").getChildFile ("plugin-scan-dead-mans-pedal.txt");
                pedal.getParentDirectory().createDirectory();
                for (auto* format : owner.pluginLibrary.formatManager().getFormats())
                {
                    juce::PluginDirectoryScanner scanner (owner.pluginLibrary.list(), *format,
                                                          format->getDefaultLocationsToSearch(), true, pedal, true);
                    juce::String name;
                    for (;;)
                    {
                        if (threadShouldExit()) return;
                        setStatusMessage ("Testing " + scanner.getNextPluginFileThatWillBeScanned().fromLastOccurrenceOf ("\\", false, false));
                        if (! scanner.scanNextFile (true, name)) break;
                        setProgress (scanner.getProgress());
                    }
                }
            }

            void threadComplete (bool cancelled) override
            {
                const int effects = owner.pluginLibrary.keepInstrumentsOnly();
                if (effects > 0) juce::Logger::writeToLog ("Plugin scan: ignored " + juce::String (effects) + " effect plugins (instruments only)");
                if (auto* settings = owner.getAppSettings()) owner.pluginLibrary.saveTo (*settings);
                const int n = owner.pluginLibrary.instruments().size();
                owner.showToast (juce::String (cancelled ? "Scan stopped. " : "Scan finished. ") + juce::String (n)
                                 + (n == 1 ? " instrument available" : " instruments available") + " -- MIXER, a LIVE strip's source button");
                juce::MessageManager::callAsync ([o = &owner] { o->pluginScan = nullptr; });
            }

        private:
            SessionComponent& owner;
        };

        pluginScan = std::make_unique<Scan> (*this);
        pluginScan->launchThread();
    }

    /** JUCE's plugin list with its scan button; the scan itself runs in a
        separate process (InstrumentHost.h), so a bad plugin cannot take the
        app down. The list is saved to the app settings when the window closes. */
    void showPluginScanner()
    {
        if (pluginListWindow != nullptr) { pluginListWindow->toFront (true); return; }

        class ListWindow final : public juce::DocumentWindow
        {
        public:
            ListWindow (SessionComponent& o, juce::PluginListComponent* content)
                : juce::DocumentWindow ("Plugins", juce::Colour (0xff0c0c17), juce::DocumentWindow::closeButton), owner (o)
            {
                setUsingNativeTitleBar (true);
                setContentOwned (content, true);
                setResizable (true, false);
                centreWithSize (720, 480);
                setVisible (true);
            }
            void closeButtonPressed() override
            {
                if (auto* settings = owner.getAppSettings()) owner.pluginLibrary.saveTo (*settings);
                owner.pluginListWindow = nullptr;   // deletes this
            }
        private:
            SessionComponent& owner;
        };

        const auto pedal = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                               .getChildFile ("EzPlay").getChildFile ("plugin-scan-dead-mans-pedal.txt");
        pedal.getParentDirectory().createDirectory();
        auto* content = new juce::PluginListComponent (pluginLibrary.formatManager(), pluginLibrary.list(), pedal, getAppSettings(), true);
        content->setSize (720, 480);
        pluginListWindow = std::make_unique<ListWindow> (*this, content);
    }

    // ---- PERFORM LIVE channel strip ----------------------------------------------

    // Every deck and every live track has one (strip index == mixer channel
    // index: 0-7 the decks, 8-11 LIVE 1-4), off until switched on.

    void showStripMenu (int i)
    {
        if (i < 0 || i >= kNumStrips) return;
        juce::PopupMenu m;
        m.addSectionHeader (stripLabel (i) + "  -  CHANNEL STRIP");
        addStripMenu (m, i, false);
        auto options = juce::PopupMenu::Options();
        if (i < mixerStrips.size() && mixerStrips[i]->isShowing()) options = options.withTargetComponent (mixerStrips[i]);
        m.showMenuAsync (options, [this, i] (int r) { handleStripMenu (i, r); });
    }

    void addStripMenu (juce::PopupMenu& into, int i, bool asSubmenu = true)
    {
        if (i < 0 || i >= kNumStrips) return;
        const bool on = stripOn[(size_t) i].load (std::memory_order_relaxed);
        juce::PopupMenu m;
        m.addItem (7001, "On", true, on);
        m.addItem (7002, "Open PERFORM LIVE...");
        m.addSeparator();
        const auto& presets = amanorsac::perform::factoryPresets();
        juce::PopupMenu byCat;
        for (const auto& cat : amanorsac::perform::presetCategoryOrder())
        {
            juce::PopupMenu sub;
            for (int p = 0; p < (int) presets.size(); ++p)
                if (presets[(size_t) p].category == cat)
                    sub.addItem (7100 + p, presets[(size_t) p].name, true, strips[(size_t) i]->presetName() == presets[(size_t) p].name);
            byCat.addSubMenu (cat, sub);
        }
        m.addSubMenu ("Preset", byCat);
        if (! asSubmenu) { into.addSubMenu ("Preset", byCat); into.addItem (7001, "On", true, on); into.addItem (7002, "Open PERFORM LIVE..."); return; }
        into.addSubMenu (juce::String ("Channel strip (PERFORM LIVE)") + (on ? juce::String (juce::CharPointer_UTF8 ("  \xe2\x97\x8f")) : juce::String()), m);
    }

    bool handleStripMenu (int i, int result)
    {
        if (i < 0 || i >= kNumStrips || result < 7001 || result >= 7400) return false;
        if (result == 7001) { setStripOn (i, ! stripOn[(size_t) i].load (std::memory_order_relaxed)); return true; }
        if (result == 7002) { if (! stripOn[(size_t) i].load (std::memory_order_relaxed)) setStripOn (i, true); openStripEditor (i); return true; }
        if (result >= 7100)
        {
            strips[(size_t) i]->loadFactory (result - 7100);
            if (! stripOn[(size_t) i].load (std::memory_order_relaxed)) setStripOn (i, true);
            showToast (stripLabel (i) + " channel strip: " + strips[(size_t) i]->presetName());
            return true;
        }
        return false;
    }

    void setStripOn (int i, bool on)
    {
        if (i < 0 || i >= kNumStrips) return;
        if (on) strips[(size_t) i]->reset();   // no stale reverb tail from last time it was on
        stripOn[(size_t) i].store (on, std::memory_order_relaxed);
        refreshColumnHeaders();
        showToast (stripLabel (i) + ": channel strip " + (on ? "on -- right-click its mixer strip for presets" : "off"));
    }

    void openStripEditor (int i)
    {
        if (i < 0 || i >= kNumStrips) return;
        auto& win = stripWindows[(size_t) i];
        if (win != nullptr) { win->toFront (true); return; }

        class StripWindow final : public juce::DocumentWindow
        {
        public:
            StripWindow (juce::AudioProcessor& p, const juce::String& title, std::function<void()> onClosed)
                : juce::DocumentWindow (title, juce::Colour (0xff0c0c17), juce::DocumentWindow::closeButton), onClose (std::move (onClosed))
            {
                setUsingNativeTitleBar (true);
                if (auto* ed = p.createEditorIfNeeded()) setContentOwned (ed, true);
                setResizable (true, false);
                centreWithSize (getWidth(), getHeight());
                setVisible (true);
            }
            void closeButtonPressed() override { if (onClose) onClose(); }
        private:
            std::function<void()> onClose;
        };

        win = std::make_unique<StripWindow> (*strips[(size_t) i], "PERFORM LIVE  --  " + stripLabel (i),
                                             [this, i] { stripWindows[(size_t) i] = nullptr; });
    }

    /** Returns true if result was one of addTrackInputMenu's items. */
    bool handleTrackInputMenu (int t, int result)
    {
        if (result == 9000) { setTrackInput (t, -1, false); return true; }
        if (result >= 9300 && result < 9400) { setTrackInput (t, result - 9300, true); return true; }
        if (result >= 9100 && result < 9300) { setTrackInput (t, result - 9100, false); return true; }
        return result == 9001;
    }

    void requestAllOutputChannels()
    {
        if (reconfiguringOutputs)
            return;

        auto* device = deviceManager.getCurrentAudioDevice();
        if (device == nullptr)
            return;

        const int available = device->getOutputChannelNames().size();
        const int open      = device->getActiveOutputChannels().countNumberOfSetBits();
        if (available <= 2 || open >= available)
            return;

        const juce::ScopedValueSetter<bool> guard (reconfiguringOutputs, true);

        auto setup = deviceManager.getAudioDeviceSetup();
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.outputChannels.setRange (0, available, true);

        // On failure we keep whatever is already open -- guaranteed at least
        // stereo by setAudioChannels(0, 2). Never fewer channels than before,
        // never a crash: the worst case is the extra outputs stay unavailable.
        const auto error = deviceManager.setAudioDeviceSetup (setup, true);
        if (error.isNotEmpty())
            juce::Logger::writeToLog ("Could not open all outputs on " + device->getName() + ": " + error);
    }

    /** Fires when the audio device is swapped, connected or reconfigured. */
    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (source == &deviceManager)
        {
            requestAllOutputChannels();
            requestLiveInputChannels();
            saveAudioDeviceState();
        }
    }

    /** Remembers the current driver/device/rate/buffer for the next launch. */
    void saveAudioDeviceState()
    {
        if (deviceManager.getCurrentAudioDevice() == nullptr)
            return;   // nothing open -- don't overwrite a good saved choice with "none"

        if (auto* settings = getAppSettings())
        {
            if (auto xml = deviceManager.createStateXml())
                settings->setValue ("audioDeviceState", xml.get());
            settings->saveIfNeeded();
        }
    }

    // SPEC_OUTPUT_ROUTING.md: how many stereo output PAIRS the currently
    // open device actually exposes (>= 1 always -- setAudioChannels(0,2) in
    // the constructor guarantees at least a stereo pair is open). Read
    // fresh from the device each call rather than cached, since the owner
    // can change audio devices at any time via the Settings > Audio tab.
    int pairCount() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return juce::jmax (1, device->getActiveOutputChannels().countNumberOfSetBits() / 2);
        return 1;
    }

    // SPEC_OUTPUT_ROUTING.md: maps MixerChannelStrip's own existing route
    // vocabulary (kOutputRoutes: "Main", "Output 1".."Output 4", "Custom
    // Bus" -- indices 0-5) onto a real output pair. "Main" is pair 0 by
    // definition; "Output N" is pair N (so "Output 1" reaches Out 3/4, the
    // device's SECOND pair, matching this app's existing "Main already
    // covers Out 1/2" framing). "Custom Bus" (index 5) has no engine-level
    // bus concept in this mixer (SPEC_OUTPUT_ROUTING.md's own option (a),
    // "probably later" for that fuller model) -- falls back to pair 0,
    // same honest-gap treatment Phase 1.1 P3's own toast already used for
    // "not real yet," not silently pretended to work.
    //
    // A pair beyond what pairCount() reports right now (e.g. a saved
    // "Output 3" on a now-stereo device) is NOT re-clamped here -- it's
    // passed straight to Mixer::setChannelOutputPair(), which stores
    // whatever's given, and Mixer::mixDown() re-clamps against the
    // CURRENT pairCount() every single block. That's deliberate: if the
    // owner reconnects the real interface later, the stored routing
    // becomes live again automatically, with no need to re-select it.
    // Selector index N == output pair N, one to one. Index 0 is Main
    // (hardware Out 1/2, through Master); 1..11 are Out 3/4 .. Out 23/24,
    // direct to hardware. Anything else clamps to Main.
    static int routeIndexToOutputPair (int routeIndex)
    {
        return (routeIndex >= 1 && routeIndex < ezproject::MixerChannelSnapshot::kMaxOutputRoutes) ? routeIndex : 0;
    }

    // Phase 1.1 P1: is it safe to mutate this deck's audio-thread-visible
    // state (Deck::mode, a layer's buffers/region/trim, seeking, etc.) right
    // now. Correction to the reasoning in the transportRunning-introducing
    // commit: getNextAudioBlock() skips calling session.renderPerTab()
    // ENTIRELY while transportRunning is false (see that function's own
    // comment) -- so while stopped, the audio thread does not read or write
    // ANY deck's fields, including decks[active]'s. That makes it safe to
    // mutate even the active deck as long as the transport isn't running,
    // not just an inactive one. isDeckAudible() alone (the old gate) is
    // still the right check while the transport IS running, since that's
    // exactly when renderPerTab() is being called every block.
    bool canMutateDeckState (int deckIdx) const
    {
        return (! session.isDeckAudible (deckIdx)) || (! transportRunning.load());
    }

    // Phase 1.1 P1: read-only mirror of transportRunning -- lets the Play/Stop
    // transport button show/gate on the SAME single flag ActionId::PlayStop
    // (registerActions(), below) now toggles, without a second, divergent
    // notion of "playing." Previously this asked "does the active deck have
    // any enabled layer" -- that conflated arming a layer with actually
    // playing, which was the root cause of "selecting a deck immediately
    // starts playback."
    bool isActiveDeckPlaying() const
    {
        return transportRunning.load();
    }

    // Milestone 14: mouse clicks kept their own existing handlers (queueSwitch
    // etc. via buttonClicked()/lambdas set up above) -- these registrations
    // exist so KEYBOARD and MIDI can invoke the exact same behavior, not to
    // replace what mouse clicks already do.
    void registerActions()
    {
        using namespace ezaction;

        // Phase 1.1 P1: PlayStop toggles the single global transportRunning
        // gate instead of mutating Layer::enabled. Layer::enabled is purely
        // a per-layer live-mute -- arming/disarming a deck card has no
        // audible effect on its own; it only matters once transportRunning
        // is true and this deck is the active one.
        //
        // Bug report: "the global play button ... has no way of stopping
        // [the pads]" -- PlayStop used to touch decks only, leaving a
        // triggered looping pad/FX voice running forever until manually
        // re-tapped. A global Stop is expected to be a real "all stop," the
        // same way it already is for decks, so stopping the transport now
        // also stops every active pad/FX voice (same primitive AllPadsOff
        // already uses below). Starting the transport does NOT auto-fire
        // any pad -- pads/FX stay a manually-triggered, independent
        // playback path when playing, matching how a physical rig's pads
        // are never retriggered just because the sequencer/transport starts.
        actionRegistry.setAction (ActionId::PlayStop, [this]
        {
            const bool nowRunning = ! transportRunning.load();
            // Owner round 4: "always start row from beginning when I pause
            // and start playing" -- Play after a stop is a full rewind
            // (master clock to bar 1, every deck playhead to 0), never a
            // resume from wherever Stop froze it. Ordered BEFORE the flag
            // flips: transportRunning is still false here, so the audio
            // thread is zero-filling (not calling session.renderPerTab) --
            // the same stopped-transport invariant canMutateDeckState()
            // already relies on -- and the seq_cst store below publishes
            // the reset before the first running block.
            if (nowRunning)
                session.resetTransport();
            transportRunning = nowRunning;
            if (! nowRunning)
            {
                for (int p = 0; p < 12; ++p) padBank.stopVoice (p);
                for (int f = 0; f < 12; ++f) fxBank.stopVoice (f);
            }
        });

        actionRegistry.setAction (ActionId::PlayNext, [this]
        {
            const int next = session.findNextDeckWithContent (signatureManager.firstDeckIndex (viewedSignature), ezdeck::kDecksPerSignature);
            if (next >= 0)
            {
                // "Play" in the name -- this action's whole purpose is to
                // advance AND start playback, unconditionally. Unlike the
                // A1-A4 trigger (SPEC_PERFORM_V2 GROUP A), this one is not
                // "just selection" -- it keeps forcing transportRunning.
                if (transportRunning.load()) session.queueSwitch (next);
                else                         session.switchNow (next);
                transportRunning = true;
            }
        });

        // SPEC_PERFORM_V2 GROUP A: same stopped-vs-playing branching as the
        // A1-A4 trigger cell -- queueSwitch() alone would never resolve
        // while the transport is stopped (render() never runs to cross the
        // bar boundary), so Prev/Next would silently do nothing visible
        // until Play was pressed. switchNow() makes the selection show
        // immediately, without starting playback.
        actionRegistry.setAction (ActionId::NextDeck, [this]
        {
            const int firstFlat = signatureManager.firstDeckIndex (viewedSignature);
            const int rawSlot = session.activeDeck() - firstFlat;
            const int currentSlot = (rawSlot >= 0 && rawSlot < kNumSlots) ? rawSlot : 0;
            const int target = flatDeckIndexForSlot ((currentSlot + 1) % kNumSlots);
            if (transportRunning.load()) session.queueSwitch (target);
            else                         session.switchNow (target);
        });
        actionRegistry.setAction (ActionId::PrevDeck, [this]
        {
            const int firstFlat = signatureManager.firstDeckIndex (viewedSignature);
            const int rawSlot = session.activeDeck() - firstFlat;
            const int currentSlot = (rawSlot >= 0 && rawSlot < kNumSlots) ? rawSlot : 0;
            const int target = flatDeckIndexForSlot ((currentSlot - 1 + kNumSlots) % kNumSlots);
            if (transportRunning.load()) session.queueSwitch (target);
            else                         session.switchNow (target);
        });

        actionRegistry.setAction (ActionId::TapTempo, [this] { handleTap(); });

        // Section playback -- same registry path as everything else, so a
        // footswitch, a key and the on-screen button all do exactly one thing.
        actionRegistry.setAction (ActionId::NextSection,  [this] { nextSection(); });
        actionRegistry.setAction (ActionId::PrevSection,  [this] { prevSection(); });
        actionRegistry.setAction (ActionId::NextSong,     [this] { nextSong(); });
        actionRegistry.setAction (ActionId::PrevSong,     [this] { prevSong(); });
        actionRegistry.setAction (ActionId::JumpNow,      [this] { jumpNow(); });
        actionRegistry.setAction (ActionId::CancelJump,   [this] { cancelJump(); });
        actionRegistry.setAction (ActionId::LoopSection,  [this] { toggleLoopSection(); });
        actionRegistry.setAction (ActionId::CountInPlay,  [this] { playWithCountIn(); });
        actionRegistry.setAction (ActionId::TogglePlaybackView, [this]
        {
            activeNavIndex = (activeNavIndex == kNavPlayback) ? 0 : kNavPlayback;
            refreshActiveView();
            if (activeNavIndex == kNavPlayback && playbackView) playbackView->songChanged();
        });

        actionRegistry.setAction (ActionId::ToggleMetronome, [this]
        {
            metronomeEnabled = ! metronomeEnabled;
            metronomeGate.store (metronomeEnabled, std::memory_order_relaxed);   // gates the loop-mode metronome only; the Click strip mute is the strip's own
        });

        actionRegistry.setAction (ActionId::ToggleTempoLock, [this]
        {
            tempoLockEnabled = ! tempoLockEnabled;
            lockToggle.setToggleState (tempoLockEnabled, juce::dontSendNotification);
            for (int d = 0; d < kNumDecks; ++d) reWarpDeckToEffectiveTempo (d);
        });

        actionRegistry.setAction (ActionId::AllPadsOff, [this]
        {
            for (int p = 0; p < 12; ++p) padBank.stopVoice (p);
        });

        // keyboard/MIDI equivalent of the trigger cell above -- must match
        // its exact behavior (Milestone 14's own convention: keyboard/MIDI
        // invoke the SAME behavior as the mouse path, never a divergent
        // one), including the trigger-always-plays correction (see
        // cell->onTrigger's own comment).
        for (int slot = 0; slot < kNumSlots; ++slot)
            actionRegistry.setAction (deckSlotAction (slot), [this, slot] { triggerRowSlot (slot); });

        for (int p = 0; p < 12; ++p)
            actionRegistry.setAction (padAction (p), [this, p] { if (padBank.voices[(size_t) p].loaded) padBank.triggerVoice (p); });

        for (int f = 0; f < 12; ++f)
            actionRegistry.setAction (fxAction (f), [this, f]
            {
                auto& v = fxBank.voices[(size_t) f];
                if (! v.loaded) return;
                if (v.loop && v.isActive()) fxBank.stopVoice (f);
                else fxBank.triggerVoice (f);
            });

        for (int s = 0; s < 8; ++s)
            actionRegistry.setAction (sceneAction (s), [this, s] { recallScene (s); });
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // Bug report: real Save mechanism -- Ctrl+S / Ctrl+Shift+S, the
        // standard shortcut, checked before the performance keymap (Save
        // isn't a PRD §15 performance action, so it's not MIDI-mappable via
        // ActionRegistry/KeyBindingMap -- see userTriggeredSave()'s own
        // comment on why it lives here instead).
        if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
        {
            saveProjectAs();
            return true;
        }
        if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0))
        {
            userTriggeredSave();
            return true;
        }

        // Owner: popular shortcuts to open the views and the docks (Ctrl on
        // Windows, Cmd on a Mac). Ctrl+1-4 follow the nav bar left to right;
        // L and M for the Library and Mixer docks, which toggle like their
        // buttons. They go through the buttons' own onClick, so a shortcut and
        // a click can never behave differently.
        {
            const auto cmd = juce::ModifierKeys::commandModifier;
            static const struct { int key; int position; } kNavShortcuts[] = {
                { '1', 0 }, { '2', 1 }, { '3', 2 }, { '4', 3 }, { 'l', kNavLibraryPos }, { 'm', kNavMixerPos }
            };
            for (const auto& s : kNavShortcuts)
                if ((key == juce::KeyPress (s.key, cmd, 0) || key == juce::KeyPress (juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) s.key), cmd, 0))
                    && s.position < navButtons.size() && navButtons[s.position]->onClick)
                {
                    navButtons[s.position]->onClick();
                    return true;
                }
        }

        // Owner #3: "delete key removes sample on deck and pads." Scoped to
        // the slot whose editor is open (the one unambiguous "selected"
        // sample), then the editor closes -- deferred, same reasoning as
        // the Hide-button crash fix.
        if (key == juce::KeyPress (juce::KeyPress::deleteKey) || key == juce::KeyPress (juce::KeyPress::backspaceKey))
        {
            if (voiceEditorContent != nullptr && editorVoiceIdx >= 0)
            {
                clearVoiceSlot (editorVoiceIsPad, editorVoiceIdx);
                juce::MessageManager::callAsync ([this] { closeEditorDock(); resized(); });
                return true;
            }
            if (clipEditorContent != nullptr && editorDeckIdx >= 0)
            {
                if (! canMutateDeckState (editorDeckIdx)) { showToast ("Stop this deck first to clear it"); return true; }
                clearLayer (editorDeckIdx, editorLayerIdx);
                refreshSlotLabels();
                repaint();
                juce::MessageManager::callAsync ([this] { closeEditorDock(); resized(); });
                return true;
            }
        }

        ezaction::ActionId action;
        if (keyBindings.actionFor (key, action)) { actionRegistry.invoke (action); return true; }
        return false;
    }

    void showToast (const juce::String& text) { toast.show (text); }

    // Milestone 11: captures the deck ACTUALLY PLAYING right now -- not
    // necessarily whichever signature is currently viewed (Milestone 6's own
    // cross-bank independence: a different bank's deck can be playing while
    // a different one is viewed). "Save current setup" means the real
    // performance state, not the on-screen one.
    void saveScene (int idx)
    {
        auto& scene = scenes[(size_t) idx];
        scene.filled = true;
        if (scene.name.isEmpty()) scene.name = "Scene " + juce::String (idx + 1);

        const int flatActive = session.activeDeck();
        scene.signatureIndex = signatureManager.signatureIndexForDeck (flatActive);
        scene.activeSlot     = flatActive - signatureManager.firstDeckIndex (scene.signatureIndex);
        scene.bpm            = masterTempo.bpm;

        for (int l = 0; l < ezdeck::kNumLayers; ++l)
            scene.tabEnabled[(size_t) l] = session.decks[(size_t) flatActive].layers[(size_t) l].enabled.load (std::memory_order_relaxed);

        for (int p = 0; p < 12; ++p)
            scene.padActive[(size_t) p] = padBank.voices[(size_t) p].isActive();

        sceneButtons[idx]->setScene (true, scene.name);
        showToast ("Saved to " + scene.name);
        repaint();
    }

    // Milestone 11: recall uses only already-established, already-safe
    // primitives -- queueSwitch() (bar-quantized, exactly like any other
    // deck activation; recall doesn't bypass this, since quantized switching
    // is this engine's own deliberate design, not a special case), setTempo(),
    // Layer::enabled (already atomic since Milestone 1), and VoiceBank's
    // atomic trigger()/requestStop(). No new Session/Deck engine code.
    void recallScene (int idx)
    {
        auto& scene = scenes[(size_t) idx];
        if (! scene.filled) { showToast (juce::String (juce::CharPointer_UTF8 ("Empty " "\xe2" "\x80" "\x94" " hold to save the current setup"))); return; }

        // Defensive bounds check -- currently unreachable (no custom
        // signature can be deleted yet, Milestone 6's own documented scope
        // boundary), but future-proofed per PRD §11's own edge case.
        if (scene.signatureIndex < 0 || scene.signatureIndex >= signatureManager.size())
        {
            showToast ("Scene's time signature no longer exists");
            return;
        }

        viewedSignature = scene.signatureIndex;
        refreshSlotLabels();

        const int flatTarget = signatureManager.firstDeckIndex (scene.signatureIndex) + scene.activeSlot;
        for (int l = 0; l < ezdeck::kNumLayers; ++l)
            session.decks[(size_t) flatTarget].layers[(size_t) l].enabled = scene.tabEnabled[(size_t) l];

        const bool tempoChanged = ! juce::approximatelyEqual (masterTempo.bpm, scene.bpm);
        masterTempo.bpm = scene.bpm;
        session.setTempo (masterTempo);

        // SPEC_PERFORM_V2 GROUP A: same stopped-vs-playing branching as
        // every other deck-selecting action -- recalling a scene while
        // stopped must show its saved deck as selected immediately
        // (switchNow), not leave the previous deck looking selected until
        // Play is eventually pressed. Recall still never forces playback
        // on its own, matching its existing (unchanged) behavior.
        if (transportRunning.load()) session.queueSwitch (flatTarget);
        else                         session.switchNow (flatTarget);

        for (int p = 0; p < 12; ++p)
        {
            if (scene.padActive[(size_t) p]) padBank.triggerVoice (p);
            else                             padBank.stopVoice (p);
        }

        juce::String msg = "Recalled " + scene.name;
        if (tempoChanged && tempoLockEnabled)
            msg += " (tempo lock overridden for this recall)";   // PRD §11's own required toast suffix
        showToast (msg);

        activeSceneIndex = idx;
        repaint();
    }

    void showSceneMenu (int idx)
    {
        auto& scene = scenes[(size_t) idx];
        juce::PopupMenu menu;
        // Owner #8: scene colour as tappable boxes at the top -- no Set Colour item.
        menu.addCustomItem (1000, std::make_unique<ColourSwatchRow> (
            scene.colourSet,
            [this, idx] (juce::uint32 argb)
            {
                scenes[(size_t) idx].colourSet  = true;
                scenes[(size_t) idx].colourArgb = argb;
                sceneButtons[idx]->setAccentColour (juce::Colour (argb));
            },
            [this, idx]
            {
                scenes[(size_t) idx].colourSet = false;
                sceneButtons[idx]->setAccentColour (juce::Colour (0xff7c5cff));
            }), nullptr);
        menu.addSeparator();
        menu.addItem (1, "Save current here");
        menu.addItem (2, "Rename", scene.filled);
        menu.addItem (3, "Clear", scene.filled);
        // "Assign MIDI/Key" arrives once Milestone 14's action registry exists.

        menu.showMenuAsync (juce::PopupMenu::Options(), [this, idx] (int result)
        {
            auto& s = scenes[(size_t) idx];
            switch (result)
            {
                case 1: saveScene (idx); return;
                case 2: promptRenameScene (idx); return;
                case 3:
                    s = Scene {};
                    sceneButtons[idx]->setScene (false, {});
                    sceneButtons[idx]->setAccentColour (juce::Colour (0xff7c5cff));
                    if (activeSceneIndex == idx) activeSceneIndex = -1;
                    showToast ("Scene cleared");
                    repaint();
                    return;
                default: return;
            }
        });
    }

    // Owner #8: the signature button's right-click is now just the shared
    // swatch row -- one tap picks a colour, no named submenu.
    // (promptColorScene is gone -- the scene menu embeds the same row.)
    void promptColorSignature (int idx)
    {
        juce::PopupMenu menu;
        menu.addCustomItem (1000, std::make_unique<ColourSwatchRow> (
            signatureColourSet[(size_t) idx],
            [this, idx] (juce::uint32 argb)
            {
                signatureColourSet[(size_t) idx]  = true;
                signatureColourArgb[(size_t) idx] = argb;
                signatureButtons[idx]->setAccentColour (juce::Colour (argb));
            },
            [this, idx]
            {
                signatureColourSet[(size_t) idx] = false;
                signatureButtons[idx]->setAccentColour (juce::Colour (0xff7c5cff));
            }), nullptr);
        menu.showMenuAsync (juce::PopupMenu::Options(), [] (int) {});
    }

    void promptRenameScene (int idx)
    {
        auto* aw = new juce::AlertWindow ("Rename Scene", "Enter a new name:", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", scenes[(size_t) idx].name);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, idx] (int result)
            {
                if (result == 1)
                {
                    const juce::String text = aw->getTextEditorContents ("name").trim();
                    if (text.isNotEmpty())
                    {
                        scenes[(size_t) idx].name = text;
                        sceneButtons[idx]->setScene (true, text);
                    }
                }
                delete aw;
            }), false);
    }

    // Milestone 13: opens the Settings panel. General's 4 toggles call back
    // into this component's OWN existing setters (setChannelMute, the same
    // lock-toggle logic buttonClicked() already uses, padBank.exclusive,
    // MixerChannelStrip::setMeterVisible) -- one path per piece of state,
    // never a second, parallel implementation just because this dialog is a
    // different entry point (ENGINEERING_PRINCIPLES.md's "one path for a
    // state change").
    void showSettingsPanel()
    {
        auto* content = new SettingsPanelContent (metronomeEnabled, tempoLockEnabled, padBank.exclusive, deviceManager,
                                                   keyBindings, [] {}, *midiRouter, openMidiDeviceNames,
                                                   [this] (float g) { mixer.setMasterGain (g); });
        content->general().onMetronomeChanged = [this] (bool enabled)
        {
            metronomeEnabled = enabled;
            metronomeGate.store (enabled, std::memory_order_relaxed);
        };
        content->general().onTempoLockChanged = [this] (bool enabled)
        {
            tempoLockEnabled = enabled;
            lockToggle.setToggleState (enabled, juce::dontSendNotification);
            for (int d = 0; d < kNumDecks; ++d) reWarpDeckToEffectiveTempo (d);
        };
        content->general().onOnePadChanged     = [this] (bool enabled) { padBank.exclusive = enabled; };

        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Settings";
        options.content.setOwned (content);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    // UI_SPEC_MIXER.md §3.1/§3.3: recomputes, for every real channel, whether
    // ANOTHER channel's solo is currently silencing it -- mirrors
    // Mixer::renderBlock()'s own "soloActive && !solo && !exemptFromSolo" law
    // exactly, read via Mixer's existing public getters (no engine change).
    // Called once at construction and after every onSoloChanged.
    void refreshMixerSoloVisuals()
    {
        bool soloActive = false;
        for (int c = 0; c < ezdeck::kNumMixerChannels; ++c)
            if (mixer.getChannelSolo ((ezdeck::MixerChannel) c)) { soloActive = true; break; }

        for (int c = 0; c < mixerStrips.size(); ++c)
        {
            const auto channel = (ezdeck::MixerChannel) c;
            const bool silenced = soloActive && ! mixer.getChannelSolo (channel) && ! mixer.isExemptFromSolo (channel);
            mixerStrips[c]->setSilencedByOtherSolo (silenced);
        }
        // Master is the post-sum stage, not one of Mixer's per-channel solo
        // participants -- never silenced by another channel's solo.
    }


    // Milestone 8/9: loads a clip from file into a VoiceBank slot (or leaves
    // the slot empty if the file doesn't exist) -- mirrors loadLayer()'s own
    // reader/decode logic exactly. Shared by both Pads and FX, since both
    // use the identical VoiceBank<12> type (PRD §9's own "structurally
    // identical to Pads").
    // UI_SPEC_PERFORM.md §2.5: "bound key top-right." Same reverse-lookup
    // the Rebind flow below already used inline -- extracted so both the
    // initial slot label and a live rebind refresh it identically.
    juce::String keyForAction (ezaction::ActionId action) const
    {
        for (auto& kv : keyBindings.all()) if (kv.second == action) return kv.first;
        return "-";
    }

    // UI_SPEC_PERFORM.md §2.5: "duration bottom-right." mm:ss, matching the
    // status bar's own plain-number convention -- no fractional seconds,
    // this is a glance-readable label, not a precise timecode.
    static juce::String formatDuration (double seconds)
    {
        const int totalSeconds = (int) std::llround (seconds);
        return juce::String (totalSeconds / 60) + ":" + (totalSeconds % 60 < 10 ? "0" : "") + juce::String (totalSeconds % 60);
    }

    void loadVoiceClip (ezdeck::VoiceBank<12>& bank, VoiceBankPanel& panel, std::array<juce::String, 12>& filePaths,
                        ezaction::ActionId action, int idx, const juce::File& file)
    {
        auto& voice = bank.voices[(size_t) idx];
        panel.setSlotBoundKey (idx, keyForAction (action));

        if (! file.existsAsFile())
        {
            panel.setSlotEmpty (idx);
            return;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
        {
            panel.setSlotEmpty (idx);
            return;
        }
        // SECURITY: validate the declared geometry before any cast or
        // allocation (eximport::readerGeometryIsSane's own comment explains
        // what a hostile header does otherwise).
        {
            juce::String reason;
            if (! eximport::readerGeometryIsSane (*reader, reason))
            {
                panel.setSlotEmpty (idx);
                showToast (file.getFileName() + ": " + reason);
                return;
            }
        }

        const int lengthSamples = (int) reader->lengthInSamples;

        // PX-A. This used to decode into a full-length scratch buffer and then
        // COPY it into the voice, so loading a pad briefly held the whole file
        // twice. Measured on a single 105 MB pad: 384 MB resident against a
        // 517 MB peak -- that 133 MB gap was this copy.
        //
        // The reader now writes straight into the vectors that become the
        // voice, and they are swapped in below: one allocation, no copy.
        //
        // Mutating the voice directly is safe here for the same reason the old
        // assign() was -- every caller has already stopped this slot. A
        // zero-length file is rejected first, because vector::data() would be
        // null and AudioBuffer requires a real pointer.
        if (lengthSamples <= 0) return;
        const bool stereo = reader->numChannels > 1;
        std::vector<float> newL, newR;
        newL.resize ((size_t) lengthSamples);
        if (stereo) newR.resize ((size_t) lengthSamples);
        {
            float* chans[2] { newL.data(), stereo ? newR.data() : nullptr };
            juce::AudioBuffer<float> dest (chans, stereo ? 2 : 1, lengthSamples);
            reader->read (&dest, 0, lengthSamples, 0, true, stereo);
        }

        voice.left.swap (newL);
        // SECURITY: `right` MUST be cleared on the mono path. It used to be
        // left untouched, so loading a mono clip over a previously-loaded
        // longer STEREO clip left a stale, shorter right channel that
        // OneShotVoice::render then indexed with the (longer) left channel's
        // position -- an out-of-bounds heap read on the audio thread,
        // leaking heap bytes straight into the output. Two ordinary drags,
        // no malformed file needed.
        if (stereo)
            voice.right.swap (newR);
        else
            voice.right.clear();
        voice.loaded = true;
        // Owner: "pads playing at a pitch higher than the key" -- tell the
        // voice the FILE's rate so render() converts to the device rate
        // (see OneShotVoice::setFileSampleRate). Decks got the same fix in
        // round 4; pads never had any conversion at all.
        voice.setFileSampleRate (reader->sampleRate);
        filePaths[(size_t) idx] = file.getFullPathName();

        const double durationSeconds = reader->sampleRate > 0.0 ? (double) lengthSamples / reader->sampleRate : 0.0;
        panel.setSlotLoaded (idx, displayFileName (file.getFileNameWithoutExtension()), formatDuration (durationSeconds));
    }

    // SPEC_PERFORM_V2 GROUP H2: Pad/FX equivalent of loadAssetIntoDeckSlot()
    // -- resolves a Library assetId to a file and reuses the EXISTING
    // loadVoiceClip() (just above) rather than a parallel load path,
    // dispatching to the pad or FX bank/panel/filePaths/asset-id-array by
    // `isPad`.
    void loadAssetIntoVoiceSlot (bool isPad, int idx, const juce::String& assetId)
    {
        auto file = ezlibrary::resolveAssetOrPath (libraryManager, assetId, juce::String());
        if (! file.existsAsFile()) { showToast ("Could not resolve that Library item"); return; }

        if (isPad)
        {
            loadVoiceClip (padBank, padPanel, padFilePaths, ezaction::padAction (idx), idx, file);
            padAssetIds[(size_t) idx] = assetId;
            showToast ("Pad " + juce::String (idx + 1) + ": loaded from Library");
        }
        else
        {
            loadVoiceClip (fxBank, fxPanel, fxFilePaths, ezaction::fxAction (idx), idx, file);
            fxAssetIds[(size_t) idx] = assetId;
            showToast ("FX " + juce::String (idx + 1) + ": loaded from Library");
        }
    }

    //== pad/FX slot operations -- one implementation each, shared by the ====
    //== right-click context menu (owner #5/#17) and the slot editor's own ===
    //== buttons (openVoiceEditor below). =====================================

    void clearVoiceSlot (bool isPad, int idx)
    {
        auto& bank         = isPad ? padBank : fxBank;
        auto& panel         = isPad ? padPanel : fxPanel;
        auto& filePaths     = isPad ? padFilePaths : fxFilePaths;
        auto& nameOverride  = isPad ? padNameOverride : fxNameOverride;
        auto& colourSet     = isPad ? padColourSet : fxColourSet;
        auto& colourArgb    = isPad ? padColourArgb : fxColourArgb;

        auto& v = bank.voices[(size_t) idx];
        v.left.clear(); v.right.clear(); v.loaded = false;
        v.setFileSampleRate (0.0);   // back to 1:1 until the next load
        filePaths[(size_t) idx].clear();
        nameOverride[(size_t) idx].clear();
        colourSet[(size_t) idx] = false;
        colourArgb[(size_t) idx] = 0;
        panel.setSlotEmpty (idx);
        panel.setSlotAccentColour (idx, false, juce::Colours::transparentBlack);
        showToast ("Cleared " + actionDisplayName (isPad ? ezaction::padAction (idx) : ezaction::fxAction (idx)));
    }

    void setVoiceSlotColour (bool isPad, int idx, bool set, juce::uint32 argb)
    {
        (isPad ? padColourSet : fxColourSet)[(size_t) idx]   = set;
        (isPad ? padColourArgb : fxColourArgb)[(size_t) idx] = argb;
        (isPad ? padPanel : fxPanel).setSlotAccentColour (idx, set, set ? juce::Colour (argb) : juce::Colours::transparentBlack);
    }

    void promptRenameVoiceSlot (bool isPad, int idx)
    {
        auto& nameOverride = isPad ? padNameOverride : fxNameOverride;
        auto& filePaths     = isPad ? padFilePaths : fxFilePaths;
        const juce::String current = nameOverride[(size_t) idx].isNotEmpty()
                                        ? nameOverride[(size_t) idx]
                                        : juce::File (filePaths[(size_t) idx]).getFileNameWithoutExtension();
        auto* aw = new juce::AlertWindow ("Rename", "Enter a name for this slot:", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", current);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, isPad, idx] (int result)
            {
                if (result == 1)
                {
                    const auto newName = aw->getTextEditorContents ("name").trim();
                    if (newName.isNotEmpty())
                    {
                        (isPad ? padNameOverride : fxNameOverride)[(size_t) idx] = newName;
                        (isPad ? padPanel : fxPanel).setSlotDisplayName (idx, newName);
                    }
                }
                delete aw;
            }), false);
    }

    void promptRebindVoiceKey (bool isPad, int idx)
    {
        const auto action = isPad ? ezaction::padAction (idx) : ezaction::fxAction (idx);
        auto& panel = isPad ? padPanel : fxPanel;
        const juce::String currentKey = keyForAction (action);
        auto* aw = new juce::AlertWindow ("Rebind " + actionDisplayName (action),
                                           "Type the key (e.g. \"q\"):", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("key", currentKey);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, action, &panel, idx] (int r)
            {
                if (r == 1)
                {
                    const auto key = juce::KeyPress::createFromDescription (aw->getTextEditorContents ("key").trim());
                    if (key != juce::KeyPress())
                    {
                        keyBindings.bind (key, action);
                        panel.setSlotBoundKey (idx, keyForAction (action));
                        showToast ("Bound " + ezaction::KeyBindingMap::keyToString (key) + " to " + actionDisplayName (action));
                    }
                }
                delete aw;
            }), false);
    }

    void promptReplaceVoiceAudio (bool isPad, int idx)
    {
        voiceEditorChooser = std::make_unique<juce::FileChooser> ("Replace audio...", juce::File(),
                                                                    "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        constexpr auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        voiceEditorChooser->launchAsync (chooserFlags, [this, isPad, idx] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (! file.existsAsFile()) return;
            const auto action = isPad ? ezaction::padAction (idx) : ezaction::fxAction (idx);
            loadVoiceClip (isPad ? padBank : fxBank, isPad ? padPanel : fxPanel,
                           isPad ? padFilePaths : fxFilePaths, action, idx, file);
            showToast (actionDisplayName (action) + ": audio replaced");
            closeEditorDock();
            resized();
        });
    }

    void armVoiceMidiLearn (bool isPad, int idx)
    {
        const auto action = isPad ? ezaction::padAction (idx) : ezaction::fxAction (idx);
        midiRouter->armLearnButton (action);
        showToast ("MIDI Learn: press a key/pad or move a control on your MIDI device to bind " + actionDisplayName (action));
    }

    // Owner #5/#17: "right-click pads and FX should open a MENU, not edit
    // (edit is double-click) -- MIDI learn needed too." Colour swatches on
    // top (owner #8), then the slot operations.
    void showVoiceSlotContextMenu (bool isPad, int idx)
    {
        auto& bank = isPad ? padBank : fxBank;
        const bool loaded = bank.voices[(size_t) idx].loaded;

        juce::PopupMenu menu;
        menu.addCustomItem (1000, std::make_unique<ColourSwatchRow> (
            (isPad ? padColourSet : fxColourSet)[(size_t) idx],
            [this, isPad, idx] (juce::uint32 argb) { setVoiceSlotColour (isPad, idx, true, argb); },
            [this, isPad, idx] { setVoiceSlotColour (isPad, idx, false, 0); }), nullptr);
        menu.addSeparator();
        menu.addItem (1, "Edit...", loaded);
        menu.addItem (2, "Rename...", loaded);
        menu.addItem (3, "Replace Audio...");
        menu.addItem (4, "Rebind Key...");
        menu.addItem (5, "MIDI Learn...");
        menu.addSeparator();
        menu.addItem (6, "Clear Slot", loaded);

        menu.showMenuAsync (juce::PopupMenu::Options(), [this, isPad, idx] (int result)
        {
            switch (result)
            {
                case 1: openVoiceEditor (isPad, idx); return;
                case 2: promptRenameVoiceSlot (isPad, idx); return;
                case 3: promptReplaceVoiceAudio (isPad, idx); return;
                case 4: promptRebindVoiceKey (isPad, idx); return;
                case 5: armVoiceMidiLearn (isPad, idx); return;
                case 6: clearVoiceSlot (isPad, idx); return;
                default: return;
            }
        });
    }

    // (Phase 1.1 P6's pack-import FileChooser flow used to live here. Owner:
    // "I don't need the + by the pads" -- the button is gone, and multi-file
    // OS drag-drop onto the slots below covers the same bulk-load intent.)

    // Owner request: OS files dropped straight onto a pad/FX slot. Loads
    // the first audio file into that slot in place (referencing, not
    // copying -- same as loadVoiceClip has always worked), extra files
    // spill into the FOLLOWING slots (the old pack-import behaviour, now a
    // gesture instead of a button), then offers a Library add for the lot.
    void loadDroppedFilesIntoVoiceSlot (bool isPad, int idx, const juce::StringArray& paths)
    {
        auto& bank        = isPad ? padBank : fxBank;
        auto& panel        = isPad ? padPanel : fxPanel;
        auto& filePaths    = isPad ? padFilePaths : fxFilePaths;

        int slot = idx, loadedCount = 0;
        for (const auto& p : paths)
        {
            if (! isSupportedAudioPath (p) || slot >= 12) continue;
            loadVoiceClip (bank, panel, filePaths,
                           isPad ? ezaction::padAction (slot) : ezaction::fxAction (slot),
                           slot, juce::File (p));
            ++slot; ++loadedCount;
        }
        if (loadedCount == 0) return;

        showToast ((isPad ? juce::String ("Pad ") : juce::String ("FX ")) + juce::String (idx + 1)
                    + (loadedCount == 1 ? ": loaded" : (": loaded " + juce::String (loadedCount) + " files")));
        offerLibraryImport (paths);
    }

    // Owner request ("when I paste an audio from Windows, ask if I want to
    // add it to the [library] too -- yes opens the metadata window, no just
    // pastes it"): one shared ask, used after every OS-file drop. Yes runs
    // the Library import (which chains the metadata editor, category
    // required); no leaves things exactly as the drop already made them.
    // `standalone` only changes the phrasing for drops that had no other
    // effect (nothing was "pasted", so no "too").
    void offerLibraryImport (const juce::StringArray& allPaths, bool standalone = false)
    {
        juce::StringArray paths;
        for (const auto& p : allPaths) if (isSupportedAudioPath (p)) paths.add (p);
        if (paths.isEmpty() || libraryTab == nullptr) return;

        const int n = paths.size();
        const juce::String what = n == 1 ? "\"" + juce::File (paths[0]).getFileName() + "\""
                                          : juce::String (n) + " files";
        auto* ask = new juce::AlertWindow ("Add to Library",
                                            "Add " + what + " to your Library" + (standalone ? "?" : " too?"),
                                            juce::MessageBoxIconType::NoIcon);
        ask->addButton ("Add to Library", 1, juce::KeyPress (juce::KeyPress::returnKey));
        ask->addButton ("No Thanks", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        ask->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, ask, paths] (int result)
            {
                delete ask;
                if (result != 1 || libraryTab == nullptr) return;
                juce::Array<juce::File> files;
                for (const auto& p : paths) files.add (juce::File (p));
                libraryTab->importFiles (files);
            }), false);
    }

    // Generic OS-file drop target -- catches a drop anywhere a more
    // specific target (deck card, pad/FX slot, Library) didn't claim it.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return anySupportedAudioPath (files); }
    void filesDropped (const juce::StringArray& files, int, int) override { offerLibraryImport (files, true); }

    // The one row-trigger implementation, shared by the trigger cell's tap
    // and the keyboard/MIDI deckSlotAction (previously two identical
    // lambdas). Owner #11: launching a row that has its own tempo makes
    // that tempo THE session tempo -- the BPM readout, the metronome
    // click, and LOCK-warping all follow the row that was just launched.
    // A row with no tempo of its own leaves the session tempo untouched
    // (and, per the effective-tempo rule, plays its samples unwarped
    // unless LOCK is engaged).
    void triggerRowSlot (int slot)
    {
        const int flat = flatDeckIndexForSlot (slot);
        // the row played from the grid gets the click/guide (see rebuildGuideSchedule)
        if (guideRowOverride != flat) { guideRowOverride = flat; rebuildGuideSchedule(); }
        if (auto t = deckTempoOverride[(size_t) flat]; t.has_value() && *t > 0.0
             && std::abs (*t - masterTempo.bpm) > 0.01)
        {
            masterTempo.bpm = *t;
            session.setTempo (masterTempo);
            if (tempoLockEnabled) reWarpDeckToEffectiveTempo (flat);
        }
        // Owner round 4 "always start row from beginning": starting from a
        // stopped transport rewinds the master clock too (bar 1, beat 1),
        // so the row starts phase-locked instead of at whatever mid-bar
        // position Stop froze. A trigger while RUNNING keeps the quantized
        // queueSwitch path untouched.
        if (transportRunning.load()) session.queueSwitch (flat);
        else                         { session.resetTransport(); session.switchNow (flat); }
        transportRunning = true;
    }

    // Milestone 12: same 3-candidate search convention searchFor() already
    // established for audio files -- if a project file already exists
    // somewhere in those locations, use it; otherwise the canonical save
    // location is C:/EzPlay/ (matching findLayerFile()'s own fallback).
    // Only ever called once, at startup, to seed currentProjectFile -- every
    // other call site reads that member instead (see its own comment), so
    // Save As can retarget where subsequent saves/autosaves land.
    //
    // PerformLive UI/UX notes: "Projects should not save as JSON files...
    // the user should only see .perform" -- the content is still plain JSON
    // (ezproject::saveToFile()/loadFromFile() are extension-agnostic, just
    // read/write whatever juce::File they're given), only the visible
    // extension changed, giving the format its own identity.
    //
    // WHERE A FRESH INSTALL PUTS ITS PROJECT. This used to fall back to a
    // hardcoded "C:/EzPlay/", which is fine on the development machine and
    // wrong everywhere else: on someone else's PC the app would create a
    // stray C:\EzPlay folder, or fail to save at all if the drive is
    // protected. Anything shipped to another person has to write somewhere
    // that person actually owns.
    //
    // So: an existing project next to the exe still wins (portable installs,
    // and the development checkout, both keep working exactly as before), and
    // only the fallback moved -- to Documents\PerformLive, where a musician
    // can find, back up and email their own work.
    juce::File computeDefaultProjectFilePath() const
    {
        // User content: Documents/Amanorsac Studio/PerformLive, never beside the
        // executable (B48/B49). There is no "look next to the exe first" any more --
        // an installed copy lives in Program Files, where the app must not write.
        return productpaths::userContent().getChildFile ("ezplay_project.perform");
    }

    // One-time migration path: a project saved by a pre-.perform build.
    // loadProjectFile() reads from this if computeDefaultProjectFilePath()'s
    // own .perform candidate doesn't exist yet -- currentProjectFile itself
    // always stays a .perform path, so the very next save (explicit or on
    // exit) writes the migrated copy out and this legacy file is never
    // touched again. Without this, upgrading to a build with this change
    // would make an existing project look like it "disappeared."
    juce::File legacyJsonProjectPath() const { return searchFor ("ezplay_project.json"); }

    juce::File projectFilePath() const { return currentProjectFile; }

    // Milestone 12: gathers every piece of Project/Settings/Scene state this
    // milestone's own runtime-state inventory classified as persistable
    // (project/MILESTONE_11-15_ARCHITECTURE.md) from this component's live
    // members into a plain-data snapshot. Deliberately mechanical --
    // ProjectFile.h's own toVar()/fromVar() round-trip is what's
    // independently tested (ProjectFileTest); this method's own job is only
    // ever "copy a field from here to there," reviewed rather than
    // exhaustively unit-tested, matching this project's established
    // precedent for JUCE-adjacent glue code.
    ezproject::ProjectSnapshot captureSnapshot() const
    {
        ezproject::ProjectSnapshot snap;

        // Settings -- Milestone 13 wired metronomeEnabled/meterVisible to
        // real behavior (Metro channel mute / MixerChannelStrip meter
        // visibility); the schema already carried these fields since
        // Milestone 12, so no file-format change was needed here.
        snap.settings.metronomeEnabled = metronomeEnabled;
        for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
        {
            snap.settings.trackInput[(size_t) t]           = trackInput[(size_t) t].load (std::memory_order_relaxed);
            snap.settings.trackStereo[(size_t) t]          = trackStereo[(size_t) t].load (std::memory_order_relaxed);
            snap.settings.trackMidiChannel[(size_t) t]     = trackMidiChannel[(size_t) t].load (std::memory_order_relaxed);
            snap.settings.trackInstrumentId[(size_t) t]    = instruments[(size_t) t].hasInstrument()
                                                               ? instruments[(size_t) t].currentDescription().createIdentifierString() : juce::String();
            snap.settings.trackInstrumentState[(size_t) t] = instruments[(size_t) t].stateAsString();
        }
        for (int i = 0; i < kNumStrips; ++i)
        {
            const bool on = stripOn[(size_t) i].load (std::memory_order_relaxed);
            snap.settings.stripOn[(size_t) i] = on;
            // Owner: "the default channel strip preset should be Stage
            // Ready." Only a strip that's switched on keeps its sound; one
            // that's off comes back as Stage Ready next time.
            if (on)
            {
                juce::MemoryBlock mb;
                strips[(size_t) i]->getStateInformation (mb);
                snap.settings.stripState[(size_t) i] = mb.toBase64Encoding();
            }
        }
        snap.settings.onePadAtATime    = padBank.exclusive;
        snap.settings.meterVisible     = true;   // no longer user-toggleable -- see SettingsGeneralTab's own comment
        snap.settings.tempoLockEnabled = tempoLockEnabled;
        snap.settings.webGain          = webGain;
        snap.settings.webMute          = webMute;

        snap.masterTempoBpm  = masterTempo.bpm;
        snap.viewedSignature = viewedSignature;
        snap.masterGain      = mixer.getMasterGain();

        for (int c = 0; c < ezdeck::kNumMixerChannels; ++c)
        {
            const auto channel = (ezdeck::MixerChannel) c;
            auto& ch = snap.mixerChannels[(size_t) c];
            ch.gain = mixer.getChannelGain (channel);
            ch.mute = mixer.getChannelMute (channel);
            ch.solo = mixer.getChannelSolo (channel);
            ch.outputRoute = channelOutputRoute[(size_t) c];
        }

        for (int d = 0; d < kNumDecks; ++d)
        {
            bool anyLoaded = false;
            for (int l = 0; l < ezdeck::kNumLayers; ++l)
                if (session.decks[(size_t) d].layers[(size_t) l].loaded) { anyLoaded = true; break; }
            if (! anyLoaded) continue;

            ezproject::DeckSnapshot ds;
            ds.flatIndex = d;
            ds.tempoOverrideBpm = deckTempoOverride[(size_t) d].has_value() ? *deckTempoOverride[(size_t) d] : -1.0;
            ds.sourceBpm      = deckSourceBpm[(size_t) d].has_value() ? *deckSourceBpm[(size_t) d] : -1.0;
            ds.stemMode       = session.decks[(size_t) d].mode == ezdeck::DeckMode::stem;
            ds.meter          = rowMeter[(size_t) d];
            ds.clickFile      = songClickTrack[(size_t) d] != nullptr ? songClickTrack[(size_t) d]->filePath : juce::String();
            ds.guideFile      = songGuideTrack[(size_t) d] != nullptr ? songGuideTrack[(size_t) d]->filePath : juce::String();
            ds.useSongClick   = useSongClick[(size_t) d];
            ds.useSongGuide   = useSongGuide[(size_t) d];
            ds.rowName        = rowName[(size_t) d];
            ds.rowColourSet   = rowColourSet[(size_t) d];
            ds.rowColourArgb  = rowColourArgb[(size_t) d];
            ds.rowNotes       = rowNotes[(size_t) d];
            ds.rowTags        = rowTags[(size_t) d];
            {
                const auto& a = arrangements[(size_t) d];
                for (const auto& sec : a.sections)
                {
                    ezproject::DeckSnapshot::SectionSnapshot ss;
                    ss.name = juce::String (juce::CharPointer_UTF8 (sec.name.c_str()));
                    ss.startBar = sec.startBar; ss.colourArgb = sec.colourArgb;
                    ss.skip = sec.skip; ss.optional = sec.optional; ss.loopOnEntry = sec.loopOnEntry; ss.pauseAfter = sec.pauseAfter;
                    ss.cue = juce::String (juce::CharPointer_UTF8 (sec.cue.c_str()));
                    ds.sections.push_back (ss);
                }
                ds.arrangementLengthBars = a.lengthBars;
                ds.endBehaviour = (int) a.atEnd;
                ds.countInBars = a.countInBars;
                ds.guideClick  = a.guideClick;
                ds.guideCues   = a.guideCues;
                ds.cueLeadBars = a.cueLeadBars;
                ds.cueCounts   = a.cueCounts;
            }
            for (int l = 0; l < ezdeck::kNumLayers; ++l)
            {
                auto& layer = session.decks[(size_t) d].layers[(size_t) l];
                auto& ls = ds.layers[(size_t) l];
                ls.filePath       = layerFilePaths[(size_t) d][(size_t) l];
                ls.assetId        = layerAssetIds[(size_t) d][(size_t) l];
                ls.regionStart    = layer.regionStart;
                ls.regionLength   = layer.regionLength;
                ls.gain           = layer.gain;
                ls.fadeInSamples  = layer.fadeInSamples;
                ls.fadeOutSamples = layer.fadeOutSamples;
                ls.trimmed        = layer.trimmed;
                ls.enabled        = layer.enabled.load (std::memory_order_relaxed);
                // SPEC_PERFORM_V2 GROUP B: slot-persistent name/colour.
                ls.nameOverride   = layerNameOverride[(size_t) d][(size_t) l];
                ls.colourSet      = layerColourSet[(size_t) d][(size_t) l];
                ls.colourArgb     = layerColourArgb[(size_t) d][(size_t) l];
                ls.linkedPad      = layerLinkedPad[(size_t) d][(size_t) l];   // SPEC_PERFORM_V2 GROUP H4
            }
            snap.decks.push_back (ds);
        }

        snap.setlist  = setlist;
        snap.jumpMode = jumpModeValue;

        for (int p = 0; p < 12; ++p)
        {
            if (! padBank.voices[(size_t) p].loaded) continue;
            ezproject::VoiceSnapshot vs;
            vs.index        = p;
            vs.filePath     = padFilePaths[(size_t) p];
            vs.assetId      = padAssetIds[(size_t) p];
            vs.loop         = padBank.voices[(size_t) p].loop;
            vs.gain         = padBank.voices[(size_t) p].gain;
            vs.enabled      = padBank.voices[(size_t) p].enabled.load (std::memory_order_relaxed);
            vs.soloed       = padBank.voices[(size_t) p].soloed.load (std::memory_order_relaxed);
            vs.nameOverride = padNameOverride[(size_t) p];
            vs.colourSet    = padColourSet[(size_t) p];
            vs.colourArgb   = padColourArgb[(size_t) p];
            snap.pads.push_back (vs);
        }
        for (int p = 0; p < 12; ++p)
        {
            if (! fxBank.voices[(size_t) p].loaded) continue;
            ezproject::VoiceSnapshot vs;
            vs.index        = p;
            vs.filePath     = fxFilePaths[(size_t) p];
            vs.assetId      = fxAssetIds[(size_t) p];
            vs.loop         = fxBank.voices[(size_t) p].loop;
            vs.gain         = fxBank.voices[(size_t) p].gain;
            vs.enabled      = fxBank.voices[(size_t) p].enabled.load (std::memory_order_relaxed);
            vs.soloed       = fxBank.voices[(size_t) p].soloed.load (std::memory_order_relaxed);
            vs.nameOverride = fxNameOverride[(size_t) p];
            vs.colourSet    = fxColourSet[(size_t) p];
            vs.colourArgb   = fxColourArgb[(size_t) p];
            snap.fx.push_back (vs);
        }

        for (int i = 0; i < 8; ++i)
        {
            auto& scene = scenes[(size_t) i];
            auto& ss = snap.scenes[(size_t) i];
            ss.filled         = scene.filled;
            ss.name           = scene.name;
            ss.signatureIndex = scene.signatureIndex;
            ss.activeSlot     = scene.activeSlot;
            ss.bpm            = scene.bpm;
            ss.tabEnabled     = scene.tabEnabled;
            ss.padActive      = scene.padActive;
            ss.colourSet      = scene.colourSet;
            ss.colourArgb     = scene.colourArgb;
        }

        snap.signatureColourSet  = signatureColourSet;
        snap.signatureColourArgb = signatureColourArgb;

        return snap;
    }

    // Milestone 12: the reverse of captureSnapshot() -- scatters a loaded
    // snapshot back into this component's live members. Loads deck/pad/FX
    // audio via the SAME loadLayer()/loadVoiceClip() functions the hardcoded
    // startup path already uses, just with persisted paths instead of the
    // a1.wav/pad1.wav convention -- one loading mechanism, two sources of
    // which files to feed it, not two different loading code paths.
    void applySnapshot (const ezproject::ProjectSnapshot& snap)
    {
        tempoLockEnabled = snap.settings.tempoLockEnabled;
        lockToggle.setToggleState (tempoLockEnabled, juce::dontSendNotification);
        padBank.exclusive = snap.settings.onePadAtATime;

        // Milestone 13: apply the EFFECTS of the two settings that have real
        // behavior behind them, not just restore the flag.
        metronomeEnabled = snap.settings.metronomeEnabled;
        webGain = snap.settings.webGain;
        webMute = snap.settings.webMute;
        if (webStrip != nullptr) { webStrip->setGainValue (webGain); webStrip->setMuteState (webMute); }
        pushWebLevel();
        for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
        {
            trackInput[(size_t) t].store (snap.settings.trackInput[(size_t) t], std::memory_order_relaxed);
            trackStereo[(size_t) t].store (snap.settings.trackStereo[(size_t) t], std::memory_order_relaxed);
            trackMidiChannel[(size_t) t].store (snap.settings.trackMidiChannel[(size_t) t], std::memory_order_relaxed);
        }
        requestLiveInputChannels();
        for (int i = 0; i < kNumStrips; ++i)
        {
            const bool on = snap.settings.stripOn[(size_t) i];
            juce::MemoryBlock mb;
            if (on && snap.settings.stripState[(size_t) i].isNotEmpty() && mb.fromBase64Encoding (snap.settings.stripState[(size_t) i]))
                strips[(size_t) i]->setStateInformation (mb.getData(), (int) mb.getSize());
            else
                strips[(size_t) i]->loadFactory (0);   // Stage Ready -- the strip's default preset
            stripOn[(size_t) i].store (on, std::memory_order_relaxed);
        }
        for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
        {
            const auto& id = snap.settings.trackInstrumentId[(size_t) t];
            const bool same = instruments[(size_t) t].hasInstrument()
                           && instruments[(size_t) t].currentDescription().createIdentifierString() == id;
            if (id.isEmpty()) { if (instruments[(size_t) t].hasInstrument()) setTrackInstrument (t, std::nullopt); continue; }
            if (same) { instruments[(size_t) t].setStateFromString (snap.settings.trackInstrumentState[(size_t) t]); continue; }
            if (auto desc = pluginLibrary.find (id)) setTrackInstrument (t, desc, snap.settings.trackInstrumentState[(size_t) t].isNotEmpty()
                                                                                    ? snap.settings.trackInstrumentState[(size_t) t] : juce::String (" "));
            else showToast (trackLabel (t) + ": its instrument is not in the plugin list on this computer -- scan for plugins, then reopen the project");
        }
        refreshTrackSourceLabels();
        refreshColumnHeaders();
        metronomeGate.store (metronomeEnabled, std::memory_order_relaxed);   // gates the loop-mode metronome only; the Click strip mute is the strip's own

        masterTempo.bpm = snap.masterTempoBpm;
        session.setTempo (masterTempo);
        viewedSignature = snap.viewedSignature;

        mixer.setMasterGain (snap.masterGain);
        for (int c = 0; c < ezdeck::kNumMixerChannels && c < (int) snap.mixerChannels.size(); ++c)
        {
            const auto channel = (ezdeck::MixerChannel) c;
            auto& ch = snap.mixerChannels[(size_t) c];
            mixer.setChannelGain (channel, ch.gain);
            mixer.setChannelMute (channel, ch.mute);
            mixer.setChannelSolo (channel, ch.solo);
            channelOutputRoute[(size_t) c] = ch.outputRoute;
            if (c < mixerStrips.size()) mixerStrips[c]->setOutputRouteIndex (ch.outputRoute);
            // SPEC_OUTPUT_ROUTING.md: restores the LIVE routing too, not
            // just the stored index/UI display -- a project reloaded with
            // Metro routed to Output 1 should still reach Output 1 without
            // the owner having to re-pick it from the dropdown.
            mixer.setChannelOutputPair (channel, routeIndexToOutputPair (ch.outputRoute));
        }
        // a saved mute on a live track would leave the mic silent with its
        // channel strip's meter still moving -- see ensureChannelAudible()
        for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
            if (trackInput[(size_t) t].load (std::memory_order_relaxed) >= 0 || instruments[(size_t) t].hasInstrument())
                ensureChannelAudible (ezdeck::kNumLayers + t);

        for (auto& ds : snap.decks)
        {
            if (ds.flatIndex < 0 || ds.flatIndex >= kNumDecks) continue;
            // mode and original tempo first: loadLayer() below reads both
            session.decks[(size_t) ds.flatIndex].mode = ds.stemMode ? ezdeck::DeckMode::stem : ezdeck::DeckMode::loop;
            if (ds.meter.isNotEmpty()) setRowMeter (ds.flatIndex, ds.meter, true);   // an unknown name is ignored
            if (ds.sourceBpm > 0.0) deckSourceBpm[(size_t) ds.flatIndex] = ds.sourceBpm;
            for (int l = 0; l < ezdeck::kNumLayers; ++l)
            {
                auto& ls = ds.layers[(size_t) l];

                // SPEC_PERFORM_V2 GROUP B: name/colour belong to the SLOT,
                // not the loop -- restored regardless of whether this
                // specific layer has an audio file, unlike the file-load
                // path below (an empty-but-named/coloured slot must
                // survive reload too, as long as the row it's in has at
                // least one OTHER loaded layer -- see captureSnapshot()'s
                // own "only decks with a loaded layer get a snapshot"
                // limitation, unchanged/pre-existing).
                layerNameOverride[(size_t) ds.flatIndex][(size_t) l] = ls.nameOverride;
                layerColourSet[(size_t) ds.flatIndex][(size_t) l]    = ls.colourSet;
                layerColourArgb[(size_t) ds.flatIndex][(size_t) l]   = ls.colourArgb;
                layerLinkedPad[(size_t) ds.flatIndex][(size_t) l]    = ls.linkedPad;   // SPEC_PERFORM_V2 GROUP H4

                if (ls.filePath.isEmpty() && ls.assetId.isEmpty()) continue;
                // Milestone 16: prefers the Library resolution (survives a
                // moved/renamed file) over the raw path when assetId is
                // present and resolves; an empty assetId (every project
                // saved before this milestone) falls straight through to
                // filePath unchanged.
                loadLayer (ds.flatIndex, l, ezlibrary::resolveAssetOrPath (libraryManager, ls.assetId, ls.filePath));
                layerAssetIds[(size_t) ds.flatIndex][(size_t) l] = ls.assetId;
                auto& layer = session.decks[(size_t) ds.flatIndex].layers[(size_t) l];
                layer.regionStart    = ls.regionStart;
                layer.regionLength   = ls.regionLength;
                layer.gain           = ls.gain;
                layer.fadeInSamples  = ls.fadeInSamples;
                layer.fadeOutSamples = ls.fadeOutSamples;
                layer.trimmed        = ls.trimmed;
                layer.enabled        = ls.enabled;
            }
            if (ds.tempoOverrideBpm > 0.0) deckTempoOverride[(size_t) ds.flatIndex] = ds.tempoOverrideBpm;
            if (ds.sourceBpm > 0.0) deckSourceBpm[(size_t) ds.flatIndex] = ds.sourceBpm;
            // the song's own click/guide tracks (SECURITY: never a remote path -- see ezlibrary::isRemotePath)
            if (ds.clickFile.isNotEmpty() && ! ezlibrary::isRemotePath (ds.clickFile)) loadSongTrack (ds.flatIndex, false, juce::File (ds.clickFile), true);
            if (ds.guideFile.isNotEmpty() && ! ezlibrary::isRemotePath (ds.guideFile)) loadSongTrack (ds.flatIndex, true,  juce::File (ds.guideFile), true);
            useSongClick[(size_t) ds.flatIndex] = ds.useSongClick;
            useSongGuide[(size_t) ds.flatIndex] = ds.useSongGuide;
            rowName[(size_t) ds.flatIndex]       = ds.rowName;
            rowColourSet[(size_t) ds.flatIndex]  = ds.rowColourSet;
            rowColourArgb[(size_t) ds.flatIndex] = ds.rowColourArgb;
            rowNotes[(size_t) ds.flatIndex]      = ds.rowNotes;
            rowTags[(size_t) ds.flatIndex]       = ds.rowTags;
            {
                auto& a = arrangements[(size_t) ds.flatIndex];
                a = ezarr::Arrangement();
                for (const auto& ss : ds.sections)
                {
                    ezarr::Section sec;
                    sec.name = ss.name.toStdString(); sec.startBar = ss.startBar; sec.colourArgb = ss.colourArgb;
                    sec.skip = ss.skip; sec.optional = ss.optional; sec.loopOnEntry = ss.loopOnEntry; sec.pauseAfter = ss.pauseAfter;
                    sec.cue = ss.cue.toStdString();
                    a.sections.push_back (sec);
                }
                a.sortSections();
                a.lengthBars  = ds.arrangementLengthBars;
                a.atEnd       = (ezarr::EndBehaviour) juce::jlimit (0, 2, ds.endBehaviour);
                a.countInBars = ds.countInBars;
                a.guideClick  = ds.guideClick;
                a.guideCues   = ds.guideCues;
                a.cueLeadBars = juce::jlimit (1, 8, ds.cueLeadBars);
                a.cueCounts   = ds.cueCounts;
            }
        }

        setlist.clear();
        for (int idx : snap.setlist)
            if (idx >= 0 && idx < kNumDecks && std::find (setlist.begin(), setlist.end(), idx) == setlist.end())
                setlist.push_back (idx);
        jumpModeValue = snap.jumpMode;
        setlistPos = setlist.empty() ? -1 : 0;
        lastSectionIdx = -1;
        queuedSectionIdx = -1;
        for (int idx : setlist) ensureArrangementLength (idx);
        if (playbackView) playbackView->songChanged();

        for (auto& vs : snap.pads)
        {
            if (vs.index < 0 || vs.index >= 12 || (vs.filePath.isEmpty() && vs.assetId.isEmpty())) continue;
            loadVoiceClip (padBank, padPanel, padFilePaths, ezaction::padAction (vs.index), vs.index, ezlibrary::resolveAssetOrPath (libraryManager, vs.assetId, vs.filePath));
            padAssetIds[(size_t) vs.index] = vs.assetId;
            padBank.voices[(size_t) vs.index].loop = vs.loop;
            padBank.voices[(size_t) vs.index].gain = vs.gain;
            padBank.voices[(size_t) vs.index].enabled.store (vs.enabled, std::memory_order_relaxed);
            padBank.voices[(size_t) vs.index].soloed.store  (vs.soloed,  std::memory_order_relaxed);
            padNameOverride[(size_t) vs.index] = vs.nameOverride;
            padColourSet[(size_t) vs.index]    = vs.colourSet;
            padColourArgb[(size_t) vs.index]   = vs.colourArgb;
            // Push the restored colour/name to the CELL too -- before this,
            // only the state arrays were restored, so a saved pad colour
            // silently vanished from the UI on every project load (barely
            // noticeable as a missing 3px stripe; fatal now that owner #6
            // fills the whole pad with it).
            padPanel.setSlotAccentColour (vs.index, vs.colourSet, juce::Colour (vs.colourArgb));
            if (vs.nameOverride.isNotEmpty()) padPanel.setSlotDisplayName (vs.index, vs.nameOverride);
        }
        for (auto& vs : snap.fx)
        {
            if (vs.index < 0 || vs.index >= 12 || (vs.filePath.isEmpty() && vs.assetId.isEmpty())) continue;
            loadVoiceClip (fxBank, fxPanel, fxFilePaths, ezaction::fxAction (vs.index), vs.index, ezlibrary::resolveAssetOrPath (libraryManager, vs.assetId, vs.filePath));
            fxAssetIds[(size_t) vs.index] = vs.assetId;
            fxBank.voices[(size_t) vs.index].loop = vs.loop;
            fxBank.voices[(size_t) vs.index].gain = vs.gain;
            fxBank.voices[(size_t) vs.index].enabled.store (vs.enabled, std::memory_order_relaxed);
            fxBank.voices[(size_t) vs.index].soloed.store  (vs.soloed,  std::memory_order_relaxed);
            fxNameOverride[(size_t) vs.index] = vs.nameOverride;
            fxColourSet[(size_t) vs.index]    = vs.colourSet;
            fxColourArgb[(size_t) vs.index]   = vs.colourArgb;
            // same cell-push as the pads loop above -- see its comment
            fxPanel.setSlotAccentColour (vs.index, vs.colourSet, juce::Colour (vs.colourArgb));
            if (vs.nameOverride.isNotEmpty()) fxPanel.setSlotDisplayName (vs.index, vs.nameOverride);
        }

        for (int i = 0; i < 8; ++i)
        {
            auto& ss = snap.scenes[(size_t) i];
            auto& scene = scenes[(size_t) i];
            scene.filled         = ss.filled;
            scene.name           = ss.name;
            scene.signatureIndex = ss.signatureIndex;
            scene.activeSlot     = ss.activeSlot;
            scene.bpm            = ss.bpm;
            scene.tabEnabled     = ss.tabEnabled;
            scene.padActive      = ss.padActive;
            scene.colourSet      = ss.colourSet;
            scene.colourArgb     = ss.colourArgb;
            sceneButtons[i]->setScene (scene.filled, scene.name);
            sceneButtons[i]->setAccentColour (scene.colourSet ? juce::Colour (scene.colourArgb) : juce::Colour (0xff7c5cff));
        }

        signatureColourSet  = snap.signatureColourSet;
        signatureColourArgb = snap.signatureColourArgb;
        for (int i = 0; i < signatureButtons.size() && i < 7; ++i)
            signatureButtons[i]->setAccentColour (signatureColourSet[(size_t) i] ? juce::Colour (signatureColourArgb[(size_t) i]) : juce::Colour (0xff7c5cff));

        refreshSlotLabels();
        rebuildGuideSchedule();   // the loaded setlist song's click and cues, before Play is ever pressed
    }

    void loadProjectFile()
    {
        ezproject::ProjectSnapshot snap;
        // .perform migration: if the new-format file isn't there yet, fall
        // back to a legacy .json one -- see legacyJsonProjectPath()'s own
        // comment. currentProjectFile (projectFilePath()) is never changed
        // here, so the next save still writes the migrated .perform copy.
        juce::File sourceFile = projectFilePath();
        if (! sourceFile.existsAsFile())
        {
            auto legacy = legacyJsonProjectPath();
            if (legacy.existsAsFile())
            {
                sourceFile = legacy;
                juce::Logger::writeToLog ("Migrating legacy ezplay_project.json -- will save as .perform from now on");
            }
            // Owner: first run (no project of the user's own yet) starts
            // from the shipped default pack instead of an empty session.
            // currentProjectFile is NOT retargeted at the pack -- the first
            // save writes the normal project path, leaving the template
            // pristine for "New from Default Pack".
            else if (auto pack = defaultPackFilePath(); pack.existsAsFile())
            {
                sourceFile = pack;
                juce::Logger::writeToLog ("First run -- starting from default_pack.perform");
            }
        }
        const bool loaded = ezproject::loadFromFile (sourceFile, snap);
        if (loaded) applySnapshot (snap);
        // no file, or it failed to parse/version-match -- the hardcoded
        // defaults already loaded earlier in the constructor stand
        // unchanged, exactly as every prior milestone's behavior was.
        juce::Logger::writeToLog (loaded ? "Loaded project file"
                                          : "No project file loaded (missing, unreadable, or version mismatch) -- using defaults");
    }

    void saveProjectFile() const
    {
        const bool ok = ezproject::saveToFile (captureSnapshot(), projectFilePath());
        // roadmap "Sprint 5: reliability" -- a clean exit supersedes the
        // autosave; deleting it here (rather than leaving it to be
        // overwritten/aged out) means offerCrashRecoveryIfNeeded() never has
        // a stale file to reason about on the next launch.
        autosaveFilePath().deleteFile();
        juce::Logger::writeToLog (ok ? "Saved project on exit" : "FAILED to save project on exit");
    }

    // PerformLive UI/UX notes: New Project / Open / Close Project all need
    // a real, unconditional reset to a blank session first -- applySnapshot()
    // only ever OVERLAYS whatever a snapshot's own (sparse) decks/pads/fx
    // list contains onto EXISTING state; it was written assuming reload
    // always starts from a freshly-constructed SessionComponent, never a
    // mid-session swap, so it never had to clear anything a snapshot
    // doesn't mention. This is that missing "clear everything" step,
    // reusing clearLayer() (the exact primitive "Clear Deck" already uses)
    // and the same per-slot clear the pad/FX "Clear slot" menu item uses,
    // rather than re-deriving what "empty" means a third time.
    void resetSessionToBlank()
    {
        for (int d = 0; d < kNumDecks; ++d)
        {
            for (int l = 0; l < ezdeck::kNumLayers; ++l)
            {
                clearLayer (d, l);
                // Unlike "Clear Deck" (which deliberately keeps the slot's
                // own name/colour, GROUP B's own convention), a brand new
                // project starts with no per-slot customisation at all.
                layerNameOverride[(size_t) d][(size_t) l] = {};
                layerColourSet[(size_t) d][(size_t) l]    = false;
                layerColourArgb[(size_t) d][(size_t) l]   = 0;
                layerLinkedPad[(size_t) d][(size_t) l]    = -1;
                layerAssetIds[(size_t) d][(size_t) l]     = {};
            }
            deckTempoOverride[(size_t) d].reset();
            deckSourceBpm[(size_t) d].reset();
            deckBufferBpm[(size_t) d] = 0.0;
            session.decks[(size_t) d].mode = ezdeck::DeckMode::stem;
            rowMeter[(size_t) d] = {};
            session.decks[(size_t) d].beatsPerBar = groupBeatsForDeck (d);
            clearSongTrack (d, false, true);
            clearSongTrack (d, true, true);
            useSongClick[(size_t) d] = true;
            useSongGuide[(size_t) d] = true;
            arrangements[(size_t) d] = ezarr::Arrangement();
            rowName[(size_t) d].clear();
            rowColourSet[(size_t) d]  = false;
            rowColourArgb[(size_t) d] = 0;
            rowNotes[(size_t) d].clear();
            rowTags[(size_t) d].clear();
        }

        for (int p = 0; p < 12; ++p)
        {
            auto& voice = padBank.voices[(size_t) p];
            voice.left.clear(); voice.right.clear(); voice.loaded = false;
            voice.enabled.store (true, std::memory_order_relaxed);
            voice.soloed.store (false, std::memory_order_relaxed);
            padFilePaths[(size_t) p].clear();
            padAssetIds[(size_t) p].clear();
            padNameOverride[(size_t) p].clear();
            padColourSet[(size_t) p]  = false;
            padColourArgb[(size_t) p] = 0;
            padPanel.setSlotEmpty (p);
        }
        for (int f = 0; f < 12; ++f)
        {
            auto& voice = fxBank.voices[(size_t) f];
            voice.left.clear(); voice.right.clear(); voice.loaded = false;
            voice.enabled.store (true, std::memory_order_relaxed);
            voice.soloed.store (false, std::memory_order_relaxed);
            fxFilePaths[(size_t) f].clear();
            fxAssetIds[(size_t) f].clear();
            fxNameOverride[(size_t) f].clear();
            fxColourSet[(size_t) f]  = false;
            fxColourArgb[(size_t) f] = 0;
            fxPanel.setSlotEmpty (f);
        }

        for (int i = 0; i < 8; ++i)
        {
            scenes[(size_t) i] = Scene{};
            sceneButtons[i]->setScene (false, {});
            sceneButtons[i]->setAccentColour (juce::Colour (performlive::kIndigo));
        }
        activeSceneIndex = -1;

        for (int c = 0; c < ezdeck::kNumMixerChannels; ++c)
        {
            const auto channel = (ezdeck::MixerChannel) c;
            mixer.setChannelGain (channel, 1.0f);
            mixer.setChannelMute (channel, false);
            mixer.setChannelSolo (channel, false);
            channelOutputRoute[(size_t) c] = 0;
            mixer.setChannelOutputPair (channel, 0);
            if (c < mixerStrips.size()) mixerStrips[c]->setOutputRouteIndex (0);
        }
        mixer.setMasterGain (1.0f);
        refreshMixerSoloVisuals();

        masterTempo.bpm = 120.0;
        session.setTempo (masterTempo);
        viewedSignature = 0;
        signatureColourSet.fill (false);
        signatureColourArgb.fill (0);
        for (int i = 0; i < signatureButtons.size(); ++i)
            signatureButtons[i]->setAccentColour (juce::Colour (performlive::kIndigo));

        metronomeEnabled = false;   // owner #11: metronome is OFF by default
        mixer.setChannelMute (ezdeck::MixerChannel::Metro, false);
        tempoLockEnabled = false;
        lockToggle.setToggleState (false, juce::dontSendNotification);
        padBank.exclusive = true;

        transportRunning = false;
        session.switchNow (0);

        refreshSlotLabels();
        refreshDeckPanelChrome();
        resized();
        repaint();
    }

    // "New Project": reset to blank, point currentProjectFile at a fresh,
    // not-yet-saved name in the same folder as the current project (or the
    // default folder if none) -- the user's very next Save/Ctrl+S picks the
    // real name via Save As only if they choose to rename it, matching how
    // desktop apps typically hand a new document an "Untitled" placeholder
    // rather than forcing a save dialog immediately.
    void newProject()
    {
        resetSessionToBlank();
        auto folder = currentProjectFile.getParentDirectory();
        currentProjectFile = folder.getNonexistentChildFile ("Untitled Project", ".perform", false);
        refreshProjectMenu();
        showToast ("New project");
    }

    // "Open...": pick a .perform file, reset to blank, then load it exactly
    // like startup does -- reusing loadProjectFile()'s own snapshot-apply
    // path rather than a second parallel "load" implementation.
    void openProjectDialog()
    {
        auto chooser = std::make_shared<juce::FileChooser> ("Open Project...", currentProjectFile, "*.perform");
        constexpr auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        chooser->launchAsync (chooserFlags, [this, chooser] (const juce::FileChooser& fc)
        {
            auto chosen = fc.getResult();
            if (! chosen.existsAsFile()) return;   // cancelled
            openProjectFile (chosen);
        });
    }

    void openProjectFile (const juce::File& file)
    {
        if (! file.existsAsFile())
        {
            showToast ("Couldn't find " + file.getFileName());
            return;
        }
        resetSessionToBlank();
        currentProjectFile = file;

        ezproject::ProjectSnapshot snap;
        if (ezproject::loadFromFile (file, snap)) applySnapshot (snap);
        else showToast ("Couldn't read " + file.getFileName() + " -- opened as blank");

        rememberRecentProject (file);
        refreshProjectMenu();
        showToast ("Opened " + file.getFileNameWithoutExtension());
    }

    // "Close Project": same reset, but returns to a fresh Untitled document
    // rather than leaving the just-closed project's content on screen --
    // there is no "no project open" empty-shell state in this app (PERFORM
    // always shows a live deck grid), so "closed" and "new" converge on the
    // same blank session, matching how most single-document creative apps
    // (this one has never supported more than one open project at a time,
    // an existing, documented boundary -- see projectSelector's own
    // constructor comment) behave when you close their only document.
    void closeProject() { newProject(); }

    // Small MRU list -- juce::PropertiesFile (JUCE's own standard per-user
    // settings store, completely separate from the PROJECT file itself) is
    // the idiomatic place for this, the same way Windows' own "recent
    // files" jump list is OS-level state, not stored inside each document.
    void rememberRecentProject (const juce::File& file)
    {
        auto* settings = getAppSettings();
        if (settings == nullptr) return;
        juce::StringArray recent;
        recent.addTokens (settings->getValue ("recentProjects"), "\n", "");
        recent.removeString (file.getFullPathName());
        recent.insert (0, file.getFullPathName());
        while (recent.size() > 6) recent.remove (recent.size() - 1);
        settings->setValue ("recentProjects", recent.joinIntoString ("\n"));
        settings->saveIfNeeded();
    }

    juce::StringArray recentProjectPaths() const
    {
        juce::StringArray recent;
        if (auto* settings = getAppSettings())
            recent.addTokens (settings->getValue ("recentProjects"), "\n", "");
        recent.removeEmptyStrings();
        return recent;
    }

    static juce::PropertiesFile* getAppSettings()
    {
        // Machine state: %LOCALAPPDATA%/Amanorsac Studio/PerformLive (File & Data
        // Conventions 1.2). An explicit file, because ApplicationProperties puts
        // it in Roaming AppData on Windows.
        static std::unique_ptr<juce::PropertiesFile> settings;
        if (settings == nullptr)
        {
            juce::PropertiesFile::Options opts;
            opts.applicationName = "PerformLive";
            opts.filenameSuffix  = "settings";
            settings = std::make_unique<juce::PropertiesFile> (productpaths::machineState().getChildFile ("PerformLive.settings"), opts);
        }
        return settings.get();
    }

    // Bug report: "there's no saving but i see some untitled project
    // somewhere" -- saveProjectFile() above only ever ran silently at exit
    // or once a minute via autosaveIfDue(); there was no user-facing,
    // confirmed Save at all. This is that action (wired to the project
    // selector's "Save" item and Ctrl+S) -- unlike the silent paths, it's
    // safe to show a modal prompt here, since it only ever runs from a
    // direct user gesture, never at shutdown or off a timer.
    //
    // Also folds in the other half of the "don't copy on import" bug report:
    // any asset this project actually references that's still stored
    // EXTERNAL (Importer.h/Library.h -- imported but never copied in) gets
    // offered for a one-time copy-into-library now, "asked when saving"
    // exactly as requested, rather than silently leaving the project
    // dependent on files that could move or be deleted later.
    void userTriggeredSave()
    {
        auto external = collectReferencedExternalAssets();
        if (external.empty())
        {
            finishUserTriggeredSave();
            return;
        }

        juce::String names;
        for (size_t i = 0; i < external.size() && i < 5; ++i)
            names += "\n" + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa2 ")) + external[i].name;
        if (external.size() > 5)
            names += "\n... and " + juce::String ((int) external.size() - 5) + " more";

        auto* aw = new juce::AlertWindow ("Copy imported files into the project?",
            juce::String ((int) external.size()) + (external.size() == 1 ? " imported file is" : " imported files are")
                + " still at their original location:" + names
                + "\n\nCopying them into the project keeps it working even if those original files are "
                  "later moved or deleted. Leaving them referenced saves disk space.",
            juce::MessageBoxIconType::QuestionIcon);
        aw->addButton ("Copy Into Project", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Keep Referencing Originals", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, external] (int result)
            {
                if (result == 1)
                {
                    int copied = 0, failed = 0;
                    for (auto entry : external)   // by value -- copyExternalEntryIntoLibrary mutates its own local copy
                    {
                        if (auto* lib = findOwningLibrary (entry.assetId))
                        {
                            if (eximport::copyExternalEntryIntoLibrary (*lib, entry)) ++copied;
                            else ++failed;
                        }
                        else ++failed;
                    }
                    if (failed > 0)
                        showToast (juce::String (copied) + " copied, " + juce::String (failed) + " failed (original file missing?)");
                }
                finishUserTriggeredSave();
                delete aw;
            }), false);
    }

    // Every asset this project's decks/pads/fx actually reference right now
    // that Library.h still has flagged external (never copied in) -- see
    // userTriggeredSave()'s own comment. Deduplicated: the same sample
    // linked into two decks is only offered once.
    std::vector<ezlibrary::LibraryEntry> collectReferencedExternalAssets() const
    {
        std::vector<ezlibrary::LibraryEntry> result;
        auto consider = [&] (const juce::String& assetId)
        {
            if (assetId.isEmpty()) return;
            for (auto& already : result) if (already.assetId == assetId) return;
            auto entry = libraryManager.findById (assetId);
            if (entry.has_value() && entry->external) result.push_back (*entry);
        };

        for (int d = 0; d < kNumDecks; ++d)
            for (int l = 0; l < ezdeck::kNumLayers; ++l)
                consider (layerAssetIds[(size_t) d][(size_t) l]);
        for (int p = 0; p < 12; ++p) consider (padAssetIds[(size_t) p]);
        for (int p = 0; p < 12; ++p) consider (fxAssetIds[(size_t) p]);

        return result;
    }

    // LibraryManager aggregates several roots but only ever exposes
    // cross-root lookups (findById()/resolve()) -- copyExternalEntryIntoLibrary()
    // needs the SPECIFIC owning Library& to copy the file into and re-save
    // its own library.json, so this finds which root actually holds assetId.
    ezlibrary::Library* findOwningLibrary (const juce::String& assetId)
    {
        for (auto& lib : libraryManager.libraries())
            if (lib->findById (assetId).has_value()) return lib.get();
        return nullptr;
    }

    void finishUserTriggeredSave()
    {
        saveProjectFile();
        refreshProjectMenu();
        showToast ("Saved " + currentProjectFile.getFileName());
    }

    // Bug report: Save As -- lets the user pick a different project file
    // instead of always silently reusing currentProjectFile. Async
    // (JUCE_MODAL_LOOPS_PERMITTED is off in this build), so the FileChooser
    // itself must stay alive until its callback fires -- kept alive by the
    // shared_ptr captured into the callback, JUCE's own documented pattern
    // for this exact situation.
    void saveProjectAs()
    {
        auto chooser = std::make_shared<juce::FileChooser> ("Save Project As...", currentProjectFile, "*.perform");
        constexpr auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting;
        chooser->launchAsync (chooserFlags, [this, chooser] (const juce::FileChooser& fc)
        {
            auto chosen = fc.getResult();
            if (chosen == juce::File()) return;   // cancelled
            if (! chosen.hasFileExtension ("perform")) chosen = chosen.withFileExtension ("perform");
            currentProjectFile = chosen;
            userTriggeredSave();
        });
    }

    // PerformLive UI/UX notes: "Support New Project / Open / Open Recent /
    // Save / Save As / Auto Save / Close Project." Rebuilds the project
    // selector's full item list -- called after startup load and after
    // every New/Open/Save/Save As, since the current filename and the
    // Recent section can both change. juce::ComboBox has no submenu
    // concept, so Recent entries are flattened into this same dropdown
    // under their own section heading rather than a nested "Open Recent >"
    // menu a ComboBox can't express.
    void refreshProjectMenu()
    {
        projectSelector.clear (juce::dontSendNotification);
        projectSelector.addItem (currentProjectFile.getFileNameWithoutExtension(), kMenuIdCurrent);
        projectSelector.addSeparator();
        projectSelector.addItem ("New Project", kMenuIdNew);
        // Owner: launch pre-filled with a default loop pack, but "can also
        // create a new empty deck that deletes all the default" -- New
        // Project above stays EMPTY; this one reloads the shipped pack.
        // Only shown when a default_pack.perform actually exists.
        if (defaultPackFilePath().existsAsFile())
            projectSelector.addItem ("New from Default Pack", kMenuIdNewDefault);
        projectSelector.addItem ("Open...", kMenuIdOpen);

        auto recent = recentProjectPaths();
        if (! recent.isEmpty())
        {
            projectSelector.addSeparator();
            projectSelector.addSectionHeading ("RECENT");
            for (int i = 0; i < recent.size(); ++i)
                projectSelector.addItem (juce::File (recent[i]).getFileNameWithoutExtension(), kMenuIdRecentBase + i);
        }

        projectSelector.addSeparator();
        projectSelector.addItem ("Save", kMenuIdSave);
        projectSelector.addItem ("Save As...", kMenuIdSaveAs);
        projectSelector.addSeparator();
        projectSelector.addItem ("Close Project", kMenuIdClose);

        projectSelector.setSelectedId (kMenuIdCurrent, juce::dontSendNotification);
    }

    // Owner: "when the app launches I will have a default set of loop pack
    // already filled in the deck." The pack is an ordinary .perform snapshot
    // named default_pack.perform, dropped in any of searchFor()'s three
    // locations (C:/EzPlay/, next to the exe, or the working directory) --
    // the owner authors it by building the session they want and using
    // Save As. First run (no project file yet) starts from it; "New from
    // Default Pack" in the project menu reloads it any time; "New Project"
    // stays EMPTY ("can also create a new empty deck that deletes all the
    // default"). The template file itself is never written to -- loading it
    // retargets saves at the normal project path.
    juce::File defaultPackFilePath() const { return searchFor ("default_pack.perform"); }

    void newProjectFromDefaultPack()
    {
        auto pack = defaultPackFilePath();
        ezproject::ProjectSnapshot snap;
        if (! pack.existsAsFile() || ! ezproject::loadFromFile (pack, snap))
        {
            showToast ("No default pack found (default_pack.perform)");
            refreshProjectMenu();
            return;
        }
        newProject();          // blank reset + fresh Untitled save target (never the pack file)
        applySnapshot (snap);
        showToast ("New project from default pack");
        refreshProjectMenu();
        resized();
        repaint();
    }

    juce::File autosaveFilePath() const { return projectFilePath().getSiblingFile ("ezplay_project.autosave.perform"); }

    // roadmap "Sprint 5: reliability" -- Autosave. Writes to a SEPARATE file
    // from the user's own explicit/exit save (saveProjectFile() above never
    // touches this one except to delete it on a clean exit), once a minute
    // while the app is open (see the 15Hz timerCallback()'s own tick-counter
    // for why this piggybacks on that timer instead of a second juce::Timer).
    // Unconditional -- no dirty-tracking exists anywhere in this codebase
    // (adding one would mean touching every mutating action in this file);
    // an unconditional write of a small JSON snapshot once a minute is cheap
    // enough that the correctness/simplicity trade is worth it over that
    // much invasive plumbing for a minor efficiency gain.
    // Owner: "when quitting, ask to save." True when what's open differs from
    // the project file on disk. Both sides go through the same toVar(), so a
    // file that simply round-trips reads as unchanged; anything unreadable
    // counts as changed, which only ever means one extra question.
    bool hasUnsavedChanges() const
    {
        ezproject::ProjectSnapshot onDisk;
        if (! ezproject::loadFromFile (projectFilePath(), onDisk)) return true;
        return juce::JSON::toString (ezproject::toVar (captureSnapshot()), true)
            != juce::JSON::toString (ezproject::toVar (onDisk), true);
    }

    /** Asks Save / Don't Save / Cancel when there are changes, then calls
        quitNow -- or does nothing on Cancel. */
    void requestQuit (std::function<void()> quitNow)
    {
        if (quitPromptOpen) return;
        if (! hasUnsavedChanges()) { quitNow(); return; }

        quitPromptOpen = true;
        auto* aw = new juce::AlertWindow ("Save changes?",
                                           "Save your changes to \"" + projectFilePath().getFileNameWithoutExtension() + "\" before quitting?\n\n"
                                           "If you don't save, the project opens next time as it was when you last saved.",
                                           juce::MessageBoxIconType::NoIcon);
        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Don't Save", 2);
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw, quitNow] (int result)
        {
            delete aw;
            quitPromptOpen = false;
            if (result == 0) return;
            if (result == 1) saveProjectFile();
            else
            {
                autosaveFilePath().deleteFile();   // discarded on purpose: no recovery offer next launch
                juce::Logger::writeToLog ("Quit without saving");
            }
            saveHandledOnExit = true;
            quitNow();
        }), false);
    }

    void autosaveIfDue()
    {
        const bool ok = ezproject::saveToFile (captureSnapshot(), autosaveFilePath());
        juce::Logger::writeToLog (ok ? "Autosaved" : "Autosave FAILED to write");
    }

    // roadmap "Sprint 5: reliability" -- Crash/project recovery. Until this,
    // the ONLY save point was save-on-exit -- a crash, force-quit, or power
    // loss lost every change since the last CLEAN exit, with no way for the
    // user (or a future diagnosis) to even know it happened. Detection is
    // simple and honest: the autosave file is newer than the main project
    // file only if some autosave happened after the last clean exit that
    // would otherwise have superseded it -- i.e. the app ran for a while
    // and did NOT exit cleanly afterward. Deferred via callAsync() from the
    // constructor (see its own call site) so the modal prompt never appears
    // before the main window is actually up.
    void offerCrashRecoveryIfNeeded()
    {
        auto autosave = autosaveFilePath();
        auto mainFile = projectFilePath();
        if (! autosave.existsAsFile()) return;
        if (mainFile.existsAsFile() && mainFile.getLastModificationTime() >= autosave.getLastModificationTime())
        {
            autosave.deleteFile();   // stale -- superseded by a clean exit since; nothing to recover
            return;
        }

        juce::Logger::writeToLog ("Autosave is newer than the last clean save -- offering recovery");

        auto* aw = new juce::AlertWindow ("Recover unsaved work?",
            "PerformLive didn't close cleanly last time. An autosave from "
                + autosave.getLastModificationTime().toString (true, true) + " is available.",
            juce::MessageBoxIconType::WarningIcon);
        aw->addButton ("Recover", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Discard", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, autosave] (int result)
            {
                if (result == 1)
                {
                    ezproject::ProjectSnapshot snap;
                    if (ezproject::loadFromFile (autosave, snap))
                    {
                        applySnapshot (snap);
                        showToast ("Recovered autosave");
                        juce::Logger::writeToLog ("Recovered from autosave");
                    }
                    else
                    {
                        showToast ("Could not read the autosave file");
                        juce::Logger::writeToLog ("Autosave recovery FAILED to parse");
                    }
                }
                else
                {
                    juce::Logger::writeToLog ("User discarded the autosave");
                }
                autosave.deleteFile();   // recovered or discarded -- stale either way
                delete aw;
            }), false);
    }

    // Milestone 6: which flat deck index (into the one shared Session) the
    // given UI slot (0..kNumSlots) currently represents -- determined by
    // whichever signature is currently viewed. The only place slot->flat
    // translation happens; every click handler above resolves through this
    // at CLICK TIME (not at widget-construction time), so switching
    // viewedSignature repoints the same 8 persistent widgets without
    // recreating them.
    int flatDeckIndexForSlot (int slot) const
    {
        return signatureManager.firstDeckIndex (viewedSignature) + slot;
    }

    // A short, unambiguous display label for a flat deck index: its
    // signature's name plus PRD §1's own side/row vocabulary (side A/B,
    // row 1-4 -- "8 decks total per signature via the A/B split").
    juce::String defaultDeckLabel (int flatDeckIdx) const
    {
        const int sigIdx = signatureManager.signatureIndexForDeck (flatDeckIdx);
        const int slot    = flatDeckIdx - signatureManager.firstDeckIndex (sigIdx);
        const juce::String side = slot < kNumSlots / 2 ? "A" : "B";
        const int row = (slot % (kNumSlots / 2)) + 1;
        return juce::String (signatureManager.signature (sigIdx).name) + " " + side + juce::String (row);
    }

    juce::String deckLabel (int flatDeckIdx) const
    {
        // Phase 1.1 P1 "Rename Row": a renamed row (e.g. "Intro") fully
        // replaces the default "4/4 A1"-style label, matching the brief's
        // own example verbatim -- every existing caller (toasts, trigger
        // cell text, stem editor header) reads this one function, so a
        // rename is picked up everywhere with no other call site changed.
        if (rowName[(size_t) flatDeckIdx].isNotEmpty()) return rowName[(size_t) flatDeckIdx];
        return defaultDeckLabel (flatDeckIdx);
    }

    // Phase 1.1 P1 terminology note: the sprint brief's "Row" (Rename Row,
    // Row Notes, "A1/A2/A3 -> Intro/Verse/Bridge/Outro") maps onto what this
    // codebase already calls a "deck" -- one flat index, one A1-4 trigger
    // cell, up to kNumLayers loaded stems (deckLabel() above already prints
    // exactly this "A1" style name). The brief's "Deck Management" (Clear
    // Deck/Duplicate Deck/Replace Audio/Rename Deck) in turn maps onto one
    // individual STEM/LAYER card within that row -- the thing this codebase
    // calls a Layer. This mapping is what "row tempo editable, individual
    // deck tempo editing disabled" in the brief's Tempo Rules section
    // already describes as existing behavior: deckTempoOverride/
    // showDeckTempoMenu already operate per flat-deck-index (per row) only
    // -- there has never been a per-layer tempo override. Flagged here as an
    // explicit interpretation, not silently assumed.
    juce::String layerDisplayName (int deckIdx, int layerIdx) const
    {
        const auto& override_ = layerNameOverride[(size_t) deckIdx][(size_t) layerIdx];
        if (override_.isNotEmpty()) return override_;
        return layerNames[(size_t) deckIdx][layerIdx];
    }

    // Re-labels/re-binds the kNumSlots persistent widgets to match whichever
    // flat deck indices viewedSignature (and now, the A/B bank / SHOW ALL
    // choice) currently map them to. Called once at startup and again every
    // time the signature rail, A/B toggle, or SHOW ALL toggle is clicked --
    // never touches session/playback state. UI_SPEC_PERFORM.md §6: this is
    // the "once when a clip loads" point for each DeckCard's waveform peak
    // cache -- setAudio() runs here, never in paint()/resized().
    void refreshSlotLabels()
    {
        for (int slot = 0; slot < kNumSlots; ++slot)
        {
            const int flat = flatDeckIndexForSlot (slot);

            for (int l = 0; l < ezdeck::kNumLayers; ++l)
            {
                auto& layer = session.decks[(size_t) flat].layers[(size_t) l];
                auto* card  = deckCards[(size_t) slot][l];

                // SPEC_PERFORM_V2 GROUP B: per-slot colour override, falling
                // back to the existing fixed columnAccent() -- re-applied
                // every refresh (not just at construction) since which flat
                // deck a given slot/column shows changes with the viewed
                // signature and A/B bank (GROUP F), and the override is
                // keyed by flat deck index, not by slot.
                card->setAccent (layerColourSet[(size_t) flat][(size_t) l]
                                  ? juce::Colour (layerColourArgb[(size_t) flat][(size_t) l])
                                  : performlive::columnAccent (l));

                if (layer.loaded)
                {
                    card->setClipName (layerDisplayName (flat, l));
                    card->setBpm (taggedBpm[(size_t) flat][l]);
                    card->setAudio (layer.left);
                }
                else
                {
                    card->setClipName ({});
                    card->setBpm (0.0);
                    card->clearAudio();
                }
            }

            const auto effective = resolveEffectiveTempo (deckTempoOverride[(size_t) flat], tempoLockEnabled, masterTempo.bpm);

            // Owner #9: big name ("A1" or the renamed row) on its own line,
            // signature on the next -- the two halves of what deckLabel()
            // used to print as one "4/4 A1" string. deckLabel() itself is
            // unchanged (toasts/menus still use the combined form).
            const juce::String side    = slot < kNumSlots / 2 ? "A" : "B";
            const int          rowNum  = (slot % (kNumSlots / 2)) + 1;
            const juce::String bigName = rowName[(size_t) flat].isNotEmpty() ? rowName[(size_t) flat]
                                                                              : side + juce::String (rowNum);
            const int sigIdx = signatureManager.signatureIndexForDeck (flat);
            triggerCells[(size_t) slot]->setSlotName (bigName);
            juce::ignoreUnused (sigIdx);
            triggerCells[(size_t) slot]->setSigText (rowMeterName (flat));   // the song's own time signature, else its group's
            triggerCells[(size_t) slot]->setRowAccentColour (rowColourSet[(size_t) flat], juce::Colour (rowColourArgb[(size_t) flat]));
            triggerCells[(size_t) slot]->setTempoText (effective.has_value()
                                                        ? (juce::String (*effective, 1) + " BPM")
                                                        : juce::String ("as rec."));
            triggerCells[(size_t) slot]->setModeIsStem (session.decks[(size_t) flat].mode == ezdeck::DeckMode::stem);
            if (slot < guideCells.size())
                guideCells[slot]->setState (songClickTrack[(size_t) flat] != nullptr, arrangements[(size_t) flat].guideClick,
                                            songGuideTrack[(size_t) flat] != nullptr, arrangements[(size_t) flat].guideCues,
                                            useSongClick[(size_t) flat], useSongGuide[(size_t) flat]);
        }
    }

    // UI_SPEC_PERFORM.md §2.4: keeps deckBankLabel's text and the A/B
    // buttons' active-state colour in sync with deckBank/showAllDeckRows.
    // Pure presentation -- no session/playback state involved.
    /** Moves the deck grid to a page of four layers. Clamped rather than
        wrapped: at the last page the right chevron simply disables, which
        reads as an edge instead of silently teleporting back to layer 1
        mid-performance. */
    void setDeckPage (int page)
    {
        const int clamped = juce::jlimit (0, kNumPages - 1, page);
        if (clamped == deckPage) return;
        deckPage = clamped;
        refreshDeckPanelChrome();
        resized();
    }

    void refreshDeckPanelChrome()
    {
        {
            juce::String dots;
            for (int i = 0; i < kNumPages; ++i)
                dots += juce::String (juce::CharPointer_UTF8 (i == deckPage ? "\xe2\x97\x8f" : "\xe2\x97\x8b")) + " ";
            pageDotsLabel.setText (dots.trim(), juce::dontSendNotification);
            pageDotsLabel.setColour (juce::Label::textColourId,
                                     juce::Colour (performlive::kIndigo));
            pagePrevButton.setEnabled (deckPage > 0);
            pageNextButton.setEnabled (deckPage < kNumPages - 1);
        }

        // Column headers name the LAYERS on this page, so page 2 reads
        // DECK 5-8 rather than repeating 1-4.
        for (int c = 0; c < kColsPerPage; ++c)
        {
            const int layer = deckPage * kColsPerPage + c;
            juce::String live;
            if (stripOn[(size_t) layer].load (std::memory_order_relaxed))
                live = (live.isEmpty() ? juce::String() : live + "  ") + "FX";
            if (columnSilenced (layer))
                live = (live.isEmpty() ? juce::String() : live + "  ") + "MUTED";
            deckColumnHeaders[(size_t) c].setText ("DECK " + juce::String (layer + 1) + (live.isEmpty() ? juce::String() : juce::String (juce::CharPointer_UTF8 ("  " "\xc2" "\xb7" " ")) + live),
                                                   juce::dontSendNotification);
            deckColumnHeaders[(size_t) c].setColour (juce::Label::textColourId,
                                                      performlive::columnAccent (layer));
        }

        deckBankLabel.setText (showAllDeckRows ? "[ ALL ]" : (deckBank == 0 ? "[ BANK A ]" : "[ BANK B ]"),
                               juce::dontSendNotification);
        bankAButton.setColour (juce::TextButton::buttonColourId,
                                (! showAllDeckRows && deckBank == 0) ? juce::Colour (performlive::kIndigo)
                                                                      : juce::Colour (performlive::kCard));
        bankBButton.setColour (juce::TextButton::buttonColourId,
                                (! showAllDeckRows && deckBank == 1) ? juce::Colour (performlive::kIndigo)
                                                                      : juce::Colour (performlive::kCard));
    }

    // UI_SPEC_PERFORM.md §2/§7 (build-order step 7): the full-screen view
    // swap itself -- one setVisible() per container, matching the
    // reference's "mutually exclusive containers" (never several partially
    // shown at once).
    // Phase 1.1 P7 "toolbar transitions": tracks the last nav index this
    // function actually animated, so the crossfade below fires exactly once
    // per real PERFORM/STORE switch -- refreshActiveView() is
    // also called (idempotently) from the LIBRARY/MIXER dock toggles and
    // the dock close buttons, which never change activeNavIndex, so those
    // calls correctly trigger no animation.
    int lastAnimatedNavIndex { 0 };

    void refreshActiveView()
    {
        const bool navChanged = (activeNavIndex != lastAnimatedNavIndex);

        // Owner: "anytime I close the browser, stop playing anything" -- a
        // video left playing behind PERFORM was the sound that kept going
        // after Stop. Leaving WEB pauses the page's players.
        if (navChanged && lastAnimatedNavIndex == kNavBrowser && browserTab) browserTab->pauseMedia();

        performScroll.setVisible   (activeNavIndex == 0);
        storeView.setVisible       (activeNavIndex == 4);
        playbackHolder.setVisible  (activeNavIndex == kNavPlayback);
        // Phase 1.1 P2/P3: LIBRARY and MIXER are docked panels now,
        // independent of activeNavIndex -- see DockHeaderBar's own comment
        // and the LIBRARY/MIXER nav buttons' own onClick above. Either can
        // be visible ALONGSIDE PERFORM/STORE (that's the whole
        // point of the dock); the nav handlers already enforce that at most
        // one of the two is open at once (one shared dock slot).
        libraryView.setVisible (libraryDockOpen);
        if (libraryDockOpen) libraryView.toFront (false);
        mixerView.setVisible (mixerDockOpen);
        if (mixerDockOpen) mixerView.toFront (false);
        editorView.setVisible (editorDockOpen);
        if (editorDockOpen) editorView.toFront (false);

        if (navChanged)
        {
            juce::Component* shown = activeNavIndex == 0 ? static_cast<juce::Component*> (&performScroll)
                                    : activeNavIndex == 4 ? static_cast<juce::Component*> (&storeView)
                                    : activeNavIndex == kNavPlayback ? static_cast<juce::Component*> (&playbackHolder)
                                                           : nullptr;
            if (shown != nullptr)
            {
                shown->setAlpha (0.0f);
                juce::Desktop::getInstance().getAnimator().fadeIn (shown, 120);
            }
            lastAnimatedNavIndex = activeNavIndex;
        }

        repaint();
    }

    ~SessionComponent() override
    {
        deviceManager.removeChangeListener (this);   // before anything else is torn down
        // Milestone 12: save-on-exit -- unless the quit prompt already saved
        // or the owner chose "Don't Save" (requestQuit()).
        if (! saveHandledOnExit) saveProjectFile();
        shutdownAudio();
        // PX-D: editors before instances, instances after the audio is stopped
        for (auto& w : instrumentWindows) w = nullptr;
        pluginListWindow = nullptr;
        for (auto& s : instruments) { s.clear(); s.collectRetired(); }
        for (auto& w : stripWindows) w = nullptr;   // editors before their processors
    }

    //== loading ==============================================================
    juce::String deckName (int deckIdx) const { return deckIdx == 0 ? "A" : "B"; }

    // Read-only bundled assets (logo.png). The folder the exe lives in comes
    // first, so an installed copy uses its own files; the working directory is
    // the development fallback. Nothing is ever written to either.
    juce::File searchFor (const juce::String& fileName) const
    {
        juce::Array<juce::File> candidates {
            productpaths::bundledResources().getChildFile (fileName),
            juce::File::getCurrentWorkingDirectory().getChildFile (fileName)
        };
        for (auto& f : candidates)
            if (f.existsAsFile()) return f;
        return {};
    }

    // Phase 1.1 P1: layerNames/layerStatus are juce::StringArray (grow-by-
    // .add()), which only stayed index-aligned with layerIdx because every
    // existing call site loaded each deck's layers strictly in order 0..3,
    // exactly once. "Replace Audio" (below) breaks that assumption -- it
    // calls loadLayer() again for an ALREADY-loaded layerIdx, and a second
    // .add() would append a 5th entry rather than overwrite the existing
    // one, shifting every later layer's name/status out of alignment. This
    // helper makes "set at this index" explicit instead of relying on
    // append-order, so loadLayer is safe to call repeatedly for the same
    // layerIdx -- a normal first-time sequential load behaves identically.
    static void setIndexed (juce::StringArray& arr, int index, const juce::String& value)
    {
        while (arr.size() <= index) arr.add ({});
        arr.set (index, value);
    }

    // Owner: "when I import files you change the name -- keep the file
    // names." New library copies keep the original filename (Importer.h's
    // uniqueLibraryFileFor), but copies made BEFORE that fix are on disk as
    // "<32-hex-assetId>_<name>.ext" -- strip that prefix for DISPLAY so the
    // owner's existing library shows real names too. Extension is kept or
    // not exactly as the caller's original string had it.
    static juce::String displayFileName (const juce::String& fileName)
    {
        if (fileName.length() > 33 && fileName[32] == '_'
             && fileName.substring (0, 32).containsOnly ("0123456789abcdefABCDEF"))
            return fileName.substring (33);
        return fileName;
    }

    void loadLayer (int deckIdx, int layerIdx, const juce::File& file)
    {
        setIndexed (layerNames[(size_t) deckIdx], layerIdx, displayFileName (file.getFileName()));

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
        {
            setIndexed (layerStatus[(size_t) deckIdx], layerIdx, "could not open " + file.getFullPathName());
            return;
        }
        // SECURITY: geometry gate before any cast/allocation -- see
        // eximport::readerGeometryIsSane.
        {
            juce::String reason;
            if (! eximport::readerGeometryIsSane (*reader, reason))
            {
                setIndexed (layerStatus[(size_t) deckIdx], layerIdx, file.getFileName() + ": " + reason);
                showToast (file.getFileName() + ": " + reason);
                return;
            }
        }

        const int lengthSamples = (int) reader->lengthInSamples;

        // PX-A, the same change as loadVoiceClip and for the same reason: this
        // decoded into a full-length scratch buffer and then copied it into the
        // layer, holding the file twice for the duration of the load.
        //
        // The staging vectors are ALSO what preserves the guarantee the copy
        // used to give for free. Nothing below touches the live layer until
        // every validation has passed, so a file rejected for exceeding the
        // stem-bar limit leaves the previous audio intact rather than
        // half-replacing it -- which is exactly what a plain read-into-the-
        // layer would have broken.
        if (lengthSamples <= 0) return;
        const bool stereo = reader->numChannels > 1;
        std::vector<float> newL, newR;
        newL.resize ((size_t) lengthSamples);
        if (stereo) newR.resize ((size_t) lengthSamples);
        {
            float* chans[2] { newL.data(), stereo ? newR.data() : nullptr };
            juce::AudioBuffer<float> dest (chans, stereo ? 2 : 1, lengthSamples);
            reader->read (&dest, 0, lengthSamples, 0, true, stereo);
        }

        auto& layer = session.decks[(size_t) deckIdx].layers[(size_t) layerIdx];
        const double duration = (double) lengthSamples / reader->sampleRate;

        // Owner #19 ("work on the overall speed of the app -- I see a couple
        // lags"): every ezdsp::analyze() below now runs over at most the
        // first 30 seconds -- the same cap (and the same tempo-is-stable
        // reasoning) Importer.h already applies. On a several-minute pad/
        // track, full-file analysis dominated the whole load and froze the
        // UI for seconds; playback still uses the full decoded buffer.
        const int analysisSamples = (int) juce::jmin ((juce::int64) lengthSamples,
                                                       (juce::int64) (reader->sampleRate * 30.0));

        // PLAN_ARRANGEMENT_VIEW.md M1: a stem-mode deck's layers keep their
        // real length (up to ezdeck::kMaxStemBars) instead of loop mode's
        // tempo-derived 4-bar region. Validated BEFORE the buffer is
        // committed to the layer below, so a rejected file never partially
        // loads (matches the "could not open" early-return above). Loop
        // mode's own path (the unchanged `else if (! layer.trimmed)` block
        // further down) is completely untouched by this branch.
        const bool stemMode = session.decks[(size_t) deckIdx].mode == ezdeck::DeckMode::stem;
        int detectedStemBars = 0;

        // Owner: the tempo is never detected from the audio -- every stem of a
        // song shares the row's one tempo. A DAW-written tempo tag is trusted
        // to pre-fill a row that has none yet; otherwise the row's tempo is
        // whatever the user set (or the master tempo, only for the bar count
        // shown on the timeline, until they set it).
        {
            const double tag = embeddedTempoFrom (*reader);
            if (tag > 0.0 && ! deckSourceBpm[(size_t) deckIdx].has_value())
            {
                deckSourceBpm[(size_t) deckIdx] = juce::jlimit (1.0, 999.0, tag);
                showToast (deckLabel (deckIdx) + ": tempo " + juce::String (tag, 1) + " BPM read from the file's tag");
            }
        }
        const double stemBpm = deckBpm (deckIdx);
        juce::ignoreUnused (analysisSamples);

        if (stemMode)
        {
            auto stemFit = ezdeck::resolveStemBarLength (duration, stemBpm);

            if (! stemFit.accepted)
            {
                setIndexed (layerStatus[(size_t) deckIdx], layerIdx, file.getFileName() + ": " + juce::String (stemFit.bars)
                    + " bars exceeds the " + juce::String (ezdeck::kMaxStemBars) + "-bar stem-mode limit -- not loaded");
                return;
            }
            detectedStemBars = stemFit.bars;
        }

        layer.left.swap (newL);
        // clear `right` on the mono path -- same stale-channel hazard as
        // loadVoiceClip's own comment (Deck::render clamps per channel so it
        // could not read OOB here, but the project-load path calls this
        // WITHOUT a preceding clearLayer, so the stale data was audible).
        if (stereo)
            layer.right.swap (newR);
        else
            layer.right.clear();
        layer.loaded = true;
        layerFilePaths[(size_t) deckIdx][(size_t) layerIdx] = file.getFullPathName();

        // roadmap "Stem Editor metadata panel": channels/bit depth are real
        // reader-reported facts (unlike key/creator/tags, which nothing in
        // this pipeline detects or stores -- those stay as the editor's own
        // honestly-labelled placeholders, not invented here).
        setIndexed (layerStatus[(size_t) deckIdx], layerIdx, juce::String (lengthSamples) + " samples @ "
                                          + juce::String (reader->sampleRate, 0) + " Hz, "
                                          + juce::String (reader->numChannels) + "ch, "
                                          + juce::String ((int) reader->bitsPerSample) + "-bit");

        if (stemMode)
        {
            // Deck::render()/renderPerTab() already ignore regionLength in
            // stem mode and play numFrames() directly (Deck.h) -- so simply
            // not fitting a 4-bar region here is sufficient for the layer to
            // play its real length today. stemBarLength records the
            // detected length for the Arrangement View's timeline (M5+); it
            // is otherwise inert this milestone -- no timeline, no playback
            // change.
            layer.stemBarLength = detectedStemBars;
            taggedBpm[(size_t) deckIdx][(size_t) layerIdx] = stemBpm;
        }
        // Owner direction ("when I upload an audio it should be UNTOUCHED
        // by default -- tempo detected but not affected, play the full
        // length"): the old auto-fitted 4-bar region is gone. regionLength
        // stays 0 = the whole file; loop mode wraps it, stem mode plays it
        // once and stops (both already the engine's own end-of-clip
        // behaviour). The editor's Fit 4 Bars / Snap Grid tools still exist
        // for when the user explicitly wants a region. Tempo is still
        // TAGGED (embedded metadata first -- owner #14 -- else detection)
        // so a later row-tempo/LOCK warp has a calibration reference, but
        // detection no longer changes what plays.
        else if (! layer.trimmed)
        {
            // the row's tempo, if the user has set one; no detection
            const double tag = deckSourceBpm[(size_t) deckIdx].value_or (0.0);
            taggedBpm[(size_t) deckIdx][(size_t) layerIdx] = tag;

            // Owner round 4: "if I haven't set the tempo of the deck there's
            // a short delay in the looping." Root cause: with no tempo set,
            // regionLength stayed 0 = loop the ENTIRE decoded file --
            // including trailing silence and, for mp3/ogg, the encoder's
            // own zero padding (~2000+ samples), all faithfully played at
            // the loop point as an audible gap. Setting a tempo made it
            // vanish because reWarp runs fitFourBars and sets a real region.
            //
            // Fix = set a LOOP POINT, not a trim: regionLength is
            // non-destructive (Deck::render keeps every sample; the editor's
            // Reset restores 0) so the untouched-by-default rule holds --
            // zero samples are modified.
            //   (a) step back over trailing silence (tempo-independent,
            //       catches encoder padding; bounded to 1.5s so a long
            //       reverb tail is never cut wholesale), then
            //   (b) floor to the last WHOLE bar from the tagged tempo (file
            //       sample rate domain -- that's what loopLength()/fmod
            //       index). Floor by integer division: never rounds past
            //       the end. Exactly-N-bars exports keep their full length.
            int loopEnd = lengthSamples;
            {
                // clamped: sampleRate is validated in range at load, but the
                // cast is kept explicitly bounded so this can never become a
                // float->int UB path again if that gate ever moves
                const int maxTrim = (int) juce::jlimit (0.0, 1.0e7, reader->sampleRate * 1.5);
                const int stopAt  = juce::jmax (0, lengthSamples - maxTrim);
                int last = lengthSamples - 1;
                auto silentAt = [&] (int i)
                {
                    // same reason as the analyse call above -- post-swap, the
                    // audio lives on the layer.
                    if (std::abs (layer.left[(size_t) i]) > 1.0e-4f) return false;
                    if (! layer.right.empty() && std::abs (layer.right[(size_t) i]) > 1.0e-4f) return false;
                    return true;
                };
                while (last >= stopAt && silentAt (last)) --last;
                loopEnd = juce::jmax (last + 1, stopAt > 0 ? stopAt : 1);
            }
            if (tag > 0.0)
            {
                const int bpb = session.decks[(size_t) deckIdx].beatsPerBar > 0
                                  ? session.decks[(size_t) deckIdx].beatsPerBar : 4;
                const int barSamples = (int) std::llround ((60.0 / tag) * bpb * reader->sampleRate);
                if (barSamples > 0 && loopEnd >= barSamples)
                    loopEnd = (loopEnd / barSamples) * barSamples;
            }
            if (loopEnd > 0 && loopEnd < lengthSamples)
                layer.setRegionLengthClamped (loopEnd);
            // layer.trimmed stays false -- editor tools / re-warp keep their
            // existing precedence over this default loop point.
        }

        // all layers in a deck share that deck's rateRatio, so it's set from
        // whichever layer in the deck loads first
        if (! deckRateSet[(size_t) deckIdx])
        {
            deckFileSampleRate[(size_t) deckIdx] = reader->sampleRate;
            deckRateSet[(size_t) deckIdx] = true;
            // Push the ratio to the deck NOW, not only in prepareToPlay():
            // previously the sole setRateRatio() call site ran at device
            // open, so any file loaded after that (i.e. every normal load)
            // played at whatever stale ratio the deck had -- for decks
            // beyond the first two that meant 1.0, i.e. a 44.1k file on a
            // 48k device played ~8.8% fast. "Native rate" must actually be
            // native.
            if (currentSampleRate > 0.0)
                session.decks[(size_t) deckIdx].setRateRatio (reader->sampleRate / currentSampleRate);
        }

        // (Removed: a second ezdsp::analyze() pass that ran only for deck 1 /
        // layer 1 and fed a whole-app detectedBpm/transientCount pair. Its
        // only consumer was the editor's "Detected tempo:" stats line, which
        // showed a DIFFERENT number from the clip's own tempo field -- the
        // confusion the owner reported. Every clip's tempo now comes from the
        // one place that computes it per-clip, just above. Deleting it also
        // removes a redundant DSP pass from that deck's load.)
    }

    // Phase 1.1 P1 "Deck Loading" / SPEC_PERFORM_V2 GROUPS B & G: the ONE
    // path that loads a Library asset into a deck slot, shared by drag-drop
    // (DeckCard::onAssetDropped), the "+"/empty-tap-then-tap-a-Library-card
    // flow (GROUP B, MySamplesTab::onCardTapMaybeLoad), and pack loading
    // (GROUP G) -- reused rather than duplicated, per the spec's own
    // "no parallel implementations" rule.
    void loadAssetIntoDeckSlot (int flat, int l, const juce::String& assetId)
    {
        if (! canMutateDeckState (flat)) { showToast (deckLabel (flat) + ": stop this deck first to load into it"); return; }

        // SPEC_PERFORM_V2 GROUP G: "a pack (a multi-loop set) loaded to a
        // row fills that row's slots." A pack is the EXISTING Library
        // collectionId concept (Phase 1.1 P2-c's stem-set auto-grouping --
        // files imported together already share one id), not a new asset
        // type -- reused, not duplicated. Checked first so dropping/tapping
        // ANY member of a pack (not just one designated "leader" entry)
        // fills the whole row, matching how collectionId grouping already
        // treats every member as equivalent elsewhere in this file.
        // loadPackIntoRow() below calls loadSingleAssetIntoDeckSlot() (NOT
        // this method) for each of its members -- otherwise every member
        // would re-trigger this same collectionId check and recurse back
        // into loadPackIntoRow() instead of actually loading.
        if (auto entry = libraryManager.findById (assetId); entry.has_value() && entry->collectionId.isNotEmpty())
        {
            loadPackIntoRow (flat, entry->collectionId);
            return;
        }

        loadSingleAssetIntoDeckSlot (flat, l, assetId);
    }

    void loadSingleAssetIntoDeckSlot (int flat, int l, const juce::String& assetId)
    {
        auto file = ezlibrary::resolveAssetOrPath (libraryManager, assetId, juce::String());
        if (! file.existsAsFile()) { showToast ("Could not resolve that Library item"); return; }

        clearLayer (flat, l);
        loadLayer (flat, l, file);
        layerAssetIds[(size_t) flat][(size_t) l] = assetId;
        refreshSlotLabels();
        showToast (deckLabel (flat) + " layer " + juce::String (l + 1) + ": loaded from Library");
        repaint();

        // Phase 1.1 P6 "Pad Relationships": "assigning a Loop automatically
        // triggers a Pad (default one-loop-to-one-pad assignment only)."
        // Scoped to this exact gesture -- dragging/loading a Library asset
        // onto a deck IS "assigning a Loop" -- not promptReplaceAudio()'s
        // deck context-menu path, which replaces an EXISTING layer's audio
        // rather than freshly assigning one. Same category substring
        // convention passesCategoryFilter() already uses ("loop"
        // case-insensitive). First empty pad slot only (the "default
        // one-...-only" reading); the user can Clear/Replace that pad
        // afterward exactly as any other pad (Phase 1.1 P6 part 1) --
        // satisfies "user may change it later" without extra state.
        if (auto entry = libraryManager.findById (assetId); entry.has_value() && entry->category.containsIgnoreCase ("loop"))
        {
            for (int p = 0; p < 12; ++p)
            {
                if (padBank.voices[(size_t) p].loaded) continue;
                loadVoiceClip (padBank, padPanel, padFilePaths, ezaction::padAction (p), p, file);
                showToast ("Loop also assigned to Pad " + juce::String (p + 1));
                break;
            }
        }
    }

    // SPEC_PERFORM_V2 GROUP G: "a pack loaded to a row fills that row's
    // slots." Collects every Library entry sharing collectionId (across
    // every root libraryManager.libraries() knows about, same iteration
    // MySamplesTab::refreshCards() already uses), sorted by name for a
    // deterministic slot order, and fills layers 0..min(4, count) via
    // loadSingleAssetIntoDeckSlot() -- the same single-layer load body
    // loadAssetIntoDeckSlot() itself uses, just without re-running that
    // method's own collectionId check (see its comment on why). Extras
    // beyond 4 are reported, not silently dropped.
    void loadPackIntoRow (int flat, const juce::String& collectionId)
    {
        std::vector<const ezlibrary::LibraryEntry*> members;
        for (auto& lib : libraryManager.libraries())
            for (auto& entry : lib->entries())
                if (entry.collectionId == collectionId)
                    members.push_back (&entry);

        std::sort (members.begin(), members.end(),
                   [] (const ezlibrary::LibraryEntry* a, const ezlibrary::LibraryEntry* b) { return a->name < b->name; });

        // Owner: a folder of stems opens the import window, which reads the
        // names, picks the click and guide out, and places up to eight.
        juce::Array<juce::File> files;
        juce::StringArray ids;
        for (auto* m : members)
        {
            const auto f = libraryManager.resolve (m->assetId);
            if (! f.existsAsFile()) continue;
            files.add (f);
            ids.add (m->assetId);
        }
        if (files.isEmpty()) { showToast (deckLabel (flat) + ": none of the folder's files could be found"); return; }
        openStemImport (flat, files, ids);
    }

    // Phase 1.1 P1 "Deck Management": unloads exactly one stem/layer (the
    // brief's "Deck"), resetting it to loadLayer()'s own defaults. Only ever
    // called on a layer canMutateDeckState() has already cleared -- callers
    // are responsible for that check (matching showDeckTempoMenu's own
    // click-time-recheck convention, since these menus are async).
    void clearLayer (int deckIdx, int layerIdx)
    {
        auto& layer = session.decks[(size_t) deckIdx].layers[(size_t) layerIdx];
        layer.left.clear();
        layer.right.clear();
        layer.loaded         = false;
        layer.enabled        = true;
        layer.regionStart    = 0;
        layer.regionLength   = 0;
        layer.gain           = 1.0f;
        layer.fadeInSamples  = 0;
        layer.fadeOutSamples = 0;
        layer.trimmed        = false;
        layer.stemBarLength  = 0;

        layerFilePaths[(size_t) deckIdx][(size_t) layerIdx].clear();
        taggedBpm[(size_t) deckIdx][(size_t) layerIdx] = 0.0;
        setIndexed (layerNames[(size_t) deckIdx], layerIdx, {});
        setIndexed (layerStatus[(size_t) deckIdx], layerIdx, {});
        // SPEC_PERFORM_V2 GROUP B: name/colour belong to the SLOT, not the
        // loop -- "Clear deck... slot keeps its name/colour" and "Clear
        // row... names/colours kept" both require this. layerNameOverride
        // used to be cleared here too (a bug relative to that spec: it also
        // silently wiped the name on every "Replace Audio", which routes
        // through this same function -- see promptReplaceAudio()). Neither
        // override is touched by clearLayer() anymore, deliberately.
    }

    // Phase 1.1 P1: pure rename -- a display-only override, never touches
    // the loaded file or its path. Same AlertWindow pattern as
    // promptSetDeckTempo() above. Available even while the deck can't be
    // mutated (renaming a label isn't audio-thread-visible state).
    void promptRenameLayer (int deckIdx, int layerIdx)
    {
        auto* aw = new juce::AlertWindow ("Rename " + deckLabel (deckIdx) + " Layer " + juce::String (layerIdx + 1),
                                           "Enter a display name, or clear the field to use the loaded file's own name:",
                                           juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", layerDisplayName (deckIdx, layerIdx));
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, deckIdx, layerIdx] (int result)
            {
                if (result == 1)
                {
                    layerNameOverride[(size_t) deckIdx][(size_t) layerIdx] = aw->getTextEditorContents ("name").trim();
                    refreshSlotLabels();
                    showToast (deckLabel (deckIdx) + " layer " + juce::String (layerIdx + 1) + ": renamed to \""
                               + layerDisplayName (deckIdx, layerIdx) + "\"");
                    repaint();
                }
                delete aw;
            }), false);
    }

    // Phase 1.1 P1: "Replace Audio" -- same async juce::FileChooser pattern
    // MySamplesTab::chooseFilesToImport() already uses, but loads straight
    // into this specific layer via the existing loadLayer() path (now safe
    // to call repeatedly for the same layerIdx -- see setIndexed()'s own
    // comment) rather than importing into the Library. Re-checks
    // canMutateDeckState() at chooser-completion time too, since the picker
    // is async and the deck could have started playing while it was open.
    std::unique_ptr<juce::FileChooser> layerFileChooser;
    void promptReplaceAudio (int deckIdx, int layerIdx)
    {
        if (! canMutateDeckState (deckIdx)) { showToast ("Stop this deck first to edit it"); return; }

        layerFileChooser = std::make_unique<juce::FileChooser> ("Replace audio...", juce::File(),
                                                                  "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        // named chooserFlags, not flags -- avoids shadowing juce::Component's
        // own internal `flags` member (MSVC C4458), same fix Sprint 6 already
        // applied to MySamplesTab::chooseFilesToImport()'s identical pattern.
        constexpr auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        layerFileChooser->launchAsync (chooserFlags, [this, deckIdx, layerIdx] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (! file.existsAsFile()) return;
            if (! canMutateDeckState (deckIdx))
            {
                showToast (deckLabel (deckIdx) + " started playing before the file was chosen -- stop it and try again");
                return;
            }
            clearLayer (deckIdx, layerIdx);
            loadLayer (deckIdx, layerIdx, file);
            refreshSlotLabels();
            showToast (deckLabel (deckIdx) + " layer " + juce::String (layerIdx + 1) + ": audio replaced");
            repaint();
        });
    }

    // Phase 1.1 P1 "Duplicate Deck": copies one loaded layer's full state
    // into the first EMPTY layer slot in the SAME row. No target picker --
    // deliberately simplified scope, flagged rather than building a full
    // drag-and-drop-style cross-row target chooser this task doesn't ask for.
    void promptDuplicateLayer (int deckIdx, int layerIdx)
    {
        if (! canMutateDeckState (deckIdx)) { showToast ("Stop this deck first to edit it"); return; }
        auto& src = session.decks[(size_t) deckIdx].layers[(size_t) layerIdx];
        if (! src.loaded) return;

        int targetIdx = -1;
        for (int l = 0; l < ezdeck::kNumLayers; ++l)
            if (l != layerIdx && ! session.decks[(size_t) deckIdx].layers[(size_t) l].loaded) { targetIdx = l; break; }

        if (targetIdx < 0) { showToast (deckLabel (deckIdx) + ": no empty layer to duplicate into"); return; }

        auto& dst = session.decks[(size_t) deckIdx].layers[(size_t) targetIdx];
        dst.left            = src.left;
        dst.right           = src.right;
        dst.loaded          = true;
        dst.enabled         = true;
        dst.regionStart     = src.regionStart;
        dst.regionLength    = src.regionLength;
        dst.gain            = src.gain;
        dst.fadeInSamples   = src.fadeInSamples;
        dst.fadeOutSamples  = src.fadeOutSamples;
        dst.trimmed         = src.trimmed;
        dst.stemBarLength   = src.stemBarLength;

        layerFilePaths[(size_t) deckIdx][(size_t) targetIdx]    = layerFilePaths[(size_t) deckIdx][(size_t) layerIdx];
        taggedBpm[(size_t) deckIdx][(size_t) targetIdx]         = taggedBpm[(size_t) deckIdx][(size_t) layerIdx];
        layerNameOverride[(size_t) deckIdx][(size_t) targetIdx] = layerDisplayName (deckIdx, layerIdx) + " copy";
        setIndexed (layerNames[(size_t) deckIdx], targetIdx, layerNames[(size_t) deckIdx][layerIdx]);
        setIndexed (layerStatus[(size_t) deckIdx], targetIdx, layerStatus[(size_t) deckIdx][layerIdx]);

        refreshSlotLabels();
        showToast (deckLabel (deckIdx) + ": layer " + juce::String (layerIdx + 1) + " duplicated into layer " + juce::String (targetIdx + 1));
        repaint();
    }

    // Phase 1.1 P1 "Clear Entire Row": unloads all kNumLayers stems of this
    // deck (the brief's "Row").
    void clearEntireRow (int deckIdx)
    {
        if (! canMutateDeckState (deckIdx)) { showToast ("Stop this deck first to edit it"); return; }
        for (int l = 0; l < ezdeck::kNumLayers; ++l)
            clearLayer (deckIdx, l);
        refreshSlotLabels();
        showToast (deckLabel (deckIdx) + ": row cleared");
        repaint();
    }

    // Phase 1.1 P1 "Duplicate Row": copies this deck's full layer set into
    // the first fully-EMPTY deck within the same bank (the 4 rows currently
    // on screen) -- same "no cross-signature/cross-bank target picker"
    // simplification as promptDuplicateLayer() above.
    void promptDuplicateRow (int deckIdx)
    {
        if (! canMutateDeckState (deckIdx)) { showToast ("Stop this deck first to edit it"); return; }

        int targetFlat = -1;
        for (int slot = 0; slot < kNumSlots; ++slot)
        {
            const int candidate = flatDeckIndexForSlot (slot);
            if (candidate == deckIdx) continue;
            bool anyLoaded = false;
            for (int l = 0; l < ezdeck::kNumLayers; ++l)
                if (session.decks[(size_t) candidate].layers[(size_t) l].loaded) { anyLoaded = true; break; }
            if (! anyLoaded) { targetFlat = candidate; break; }
        }

        if (targetFlat < 0) { showToast ("No empty row on screen to duplicate into"); return; }
        if (! canMutateDeckState (targetFlat)) { showToast (deckLabel (targetFlat) + " isn't safe to write to right now"); return; }

        for (int l = 0; l < ezdeck::kNumLayers; ++l)
        {
            auto& src = session.decks[(size_t) deckIdx].layers[(size_t) l];
            if (! src.loaded) continue;
            auto& dst = session.decks[(size_t) targetFlat].layers[(size_t) l];
            dst.left            = src.left;
            dst.right           = src.right;
            dst.loaded          = true;
            dst.enabled         = true;
            dst.regionStart     = src.regionStart;
            dst.regionLength    = src.regionLength;
            dst.gain            = src.gain;
            dst.fadeInSamples   = src.fadeInSamples;
            dst.fadeOutSamples  = src.fadeOutSamples;
            dst.trimmed         = src.trimmed;
            dst.stemBarLength   = src.stemBarLength;

            layerFilePaths[(size_t) targetFlat][(size_t) l]    = layerFilePaths[(size_t) deckIdx][(size_t) l];
            taggedBpm[(size_t) targetFlat][(size_t) l]         = taggedBpm[(size_t) deckIdx][(size_t) l];
            deckSourceBpm[(size_t) targetFlat]                 = deckSourceBpm[(size_t) deckIdx];
            deckBufferBpm[(size_t) targetFlat]                 = deckBufferBpm[(size_t) deckIdx];
            if (rowMeter[(size_t) deckIdx].isNotEmpty()) setRowMeter (targetFlat, rowMeter[(size_t) deckIdx], true);
            layerNameOverride[(size_t) targetFlat][(size_t) l] = layerDisplayName (deckIdx, l);
            setIndexed (layerNames[(size_t) targetFlat], l, layerNames[(size_t) deckIdx][l]);
            setIndexed (layerStatus[(size_t) targetFlat], l, layerStatus[(size_t) deckIdx][l]);
        }

        refreshSlotLabels();
        showToast (deckLabel (deckIdx) + ": row duplicated into " + deckLabel (targetFlat));
        repaint();
    }

    // Phase 1.1 P1 "Deck Management" context menu -- one individual stem/
    // layer card's long-press/right-click (was showDeckTempoMenu, the
    // ROW-level tempo/mode menu; that stays reachable from the trigger
    // cell's own onMenu, unchanged). canMutateDeckState() gates every item
    // that touches audio-thread-visible state; Rename is always available.
    void showLayerContextMenu (int deckIdx, int layerIdx)
    {
        auto& layer = session.decks[(size_t) deckIdx].layers[(size_t) layerIdx];
        const bool canMutate = canMutateDeckState (deckIdx);

        juce::PopupMenu menu;
        // Owner #8: colours as tappable boxes at the top -- no Set Colour item.
        menu.addCustomItem (1000, std::make_unique<ColourSwatchRow> (
            layerColourSet[(size_t) deckIdx][(size_t) layerIdx],
            [this, deckIdx, layerIdx] (juce::uint32 argb)
            {
                layerColourSet[(size_t) deckIdx][(size_t) layerIdx]  = true;
                layerColourArgb[(size_t) deckIdx][(size_t) layerIdx] = argb;
                refreshSlotLabels(); repaint();
            },
            [this, deckIdx, layerIdx]
            {
                layerColourSet[(size_t) deckIdx][(size_t) layerIdx] = false;
                refreshSlotLabels(); repaint();
            }), nullptr);
        menu.addSeparator();
        menu.addItem (1, "Rename Deck...", layer.loaded);
        menu.addItem (8, "Link Pad...", layer.loaded);     // SPEC_PERFORM_V2 GROUP H4
        menu.addItem (2, "Replace Audio...", canMutate);
        menu.addItem (3, "Clear Deck", layer.loaded && canMutate);
        addStripMenu (menu, layerIdx);       // PERFORM LIVE channel strip (mics and instruments live on the mixer's LIVE tracks)
        menu.addSeparator();
        // Owner request: "link to MIDI device from the right-click menu for
        // pads and deck" -- arms the router's existing learn mode for this
        // ROW's trigger action (the same deckSlotAction the keyboard row
        // triggers already invoke; rows are the deck-level trigger unit).
        menu.addItem (9, "MIDI Learn (row trigger)...");
        menu.addSeparator();
        menu.addItem (4, "Duplicate Deck", layer.loaded && canMutate);
        menu.addSeparator();
        menu.addItem (5, "Clear Entire Row", canMutate);
        menu.addItem (6, "Duplicate Row", canMutate);

        menu.showMenuAsync (juce::PopupMenu::Options(), [this, deckIdx, layerIdx] (int result)
        {
            if (handleStripMenu (layerIdx, result)) return;
            switch (result)
            {
                case 1: promptRenameLayer (deckIdx, layerIdx); return;
                case 8: promptLinkPad (deckIdx, layerIdx); return;
                case 2: promptReplaceAudio (deckIdx, layerIdx); return;
                case 3:
                    if (! canMutateDeckState (deckIdx)) { showToast ("Stop this deck first to edit it"); return; }
                    clearLayer (deckIdx, layerIdx);
                    refreshSlotLabels();
                    showToast (deckLabel (deckIdx) + " layer " + juce::String (layerIdx + 1) + " cleared");
                    repaint();
                    return;
                case 4: promptDuplicateLayer (deckIdx, layerIdx); return;
                case 5: clearEntireRow (deckIdx); return;
                case 6: promptDuplicateRow (deckIdx); return;
                case 9:
                {
                    const int sigIdx = signatureManager.signatureIndexForDeck (deckIdx);
                    const int slot   = deckIdx - signatureManager.firstDeckIndex (sigIdx);
                    midiRouter->armLearnButton (ezaction::deckSlotAction (slot));
                    showToast ("MIDI Learn: press a key/pad on your MIDI device to trigger " + deckLabel (deckIdx));
                    return;
                }
                default: return;
            }
        });
    }

    // SPEC_PERFORM_V2 GROUP H4: "a loop can be linked to one pad... set via
    // right-click the loop -> 'Link pad'." Lists Pad 1..12 with a checkmark
    // on the currently-linked pad (if any), plus Unlink. Stored in
    // layerLinkedPad[deckIdx][layerIdx] (-1 = no link); auto-fire logic
    // lives in timerCallback() below, and persistence in LayerSnapshot's
    // own linkedPad field.
    void promptLinkPad (int deckIdx, int layerIdx)
    {
        const int current = layerLinkedPad[(size_t) deckIdx][(size_t) layerIdx];

        juce::PopupMenu menu;
        for (int p = 0; p < 12; ++p)
            menu.addItem (p + 1, "Pad " + juce::String (p + 1), true, p == current);
        menu.addSeparator();
        const int unlinkId = 13;
        menu.addItem (unlinkId, "Unlink", current >= 0);

        menu.showMenuAsync (juce::PopupMenu::Options(), [this, deckIdx, layerIdx, unlinkId] (int result)
        {
            if (result == 0) return;
            if (result == unlinkId)
            {
                layerLinkedPad[(size_t) deckIdx][(size_t) layerIdx] = -1;
                showToast (deckLabel (deckIdx) + " layer " + juce::String (layerIdx + 1) + ": pad link removed");
            }
            else
            {
                const int pad = result - 1;
                layerLinkedPad[(size_t) deckIdx][(size_t) layerIdx] = pad;
                showToast (deckLabel (deckIdx) + " layer " + juce::String (layerIdx + 1) + ": linked to Pad " + juce::String (pad + 1));
            }
        });
    }

    // (promptColorLayer/promptColorRow are gone -- owner #8: every menu now
    // embeds the shared ColourSwatchRow at its top instead.)

    //== clip editor ===========================================================
    // Opens the editor for a loaded layer: name, basic stats, and a waveform
    // of its actual audio (M3-T4/M3-T5), closed via "Done". No-op for an
    // empty slot. launchAsync() launches the
    // window modally and returns immediately (runModal()'s nested message
    // loop is disabled in this build via JUCE_MODAL_LOOPS_PERMITTED, and is
    // the discouraged option even when available, per JUCE's own docs); the
    // window auto-deletes itself when its modal state ends. The audio thread
    // itself is unaffected merely by the editor being open -- but since
    // M3-T6, dragging the region-end handle IS a real cross-thread write to
    // this layer's regionLength/trimmed; see WaveformView's header comment
    // for the data-race caveat this hasn't yet been made safe against.
    // PerformLive UI/UX Design Notes (Studio One reference): "never floating
    // windows... same bottom panel" -- this used to launch a
    // juce::DialogWindow; now it hosts ClipEditorContent inside the shared
    // editorView dock slot instead, closing Library/Mixer first (only one
    // dock panel visible at a time). closeEditorDock() is the exact reverse.
    void openClipEditor (int deckIdx, int layerIdx)
    {
        auto& layer = session.decks[(size_t) deckIdx].layers[(size_t) layerIdx];
        if (! layer.loaded) return;

        const juce::String name  = layerDisplayName (deckIdx, layerIdx);
        // Owner: "what is tagged tempo? we should only have detected tempo."
        // There used to be a SECOND tempo printed here -- the whole-app
        // `detectedBpm` member, computed only ever for deck 1 / layer 1, and
        // unrelated to the clip actually being edited. On any other slot it
        // showed nothing; on that one slot it showed a DIFFERENT number from
        // the clip's own tempo field right above it (a file carrying an
        // embedded tempo tag would read e.g. 105 in the field and 140 here).
        // Gone: the editor now shows exactly one tempo, the clip's own.
        const juce::String stats = layerStatus[(size_t) deckIdx][layerIdx];

        clipEditorContent.reset();   // drop any previously-open clip first -- see its dtor for why this is safe (stops its own preview)
        voiceEditorContent.reset();  // only one of the two editor types is ever hosted in editorView at once
        clipEditorContent = std::make_unique<ClipEditorContent> (
            name, stats, layer,
            taggedBpm[(size_t) deckIdx][(size_t) layerIdx],
            deckFileSampleRate[(size_t) deckIdx],
            performlive::columnAccent (layerIdx),
            [this, deckIdx, layerIdx] (int startSample, bool loop, bool solo)
            {
                auto& previewLayer = session.decks[(size_t) deckIdx].layers[(size_t) layerIdx];
                if (solo) startPreviewSolo (deckIdx, layerIdx);
                startPreview (previewLayer, startSample, session.decks[(size_t) deckIdx].getRateRatio(), loop);
            },
            [this] { stopPreview(); },
            [this] { return preview.active.load (std::memory_order_relaxed); },
            // Owner crash report ("anytime I click Hide the app shuts
            // down"): this onClose is invoked from the editor's OWN Hide
            // button, and closeEditorDock() destroys the editor -- deleting
            // the component whose button callback is still on the stack is
            // a use-after-free the moment the click unwinds. callAsync
            // defers the teardown to the next message-loop tick, after the
            // click has fully returned. (The dock header's own x button is
            // NOT inside the editor, so it closes synchronously, unchanged.)
            [this] { juce::MessageManager::callAsync ([this] { closeEditorDock(); resized(); }); },
            // onLayerEdited: not used by this editor instance (the deck lane
            // repaints from the same shared Layer), kept as the default.
            std::function<void()>(),
            [this] (int sample) { scrubPreviewTo (sample); },
            // Owner: "I should see the playhead moving in the editor."
            // SessionComponent is the only place that knows BOTH sources:
            // the editor's own preview voice (which wins while auditioning)
            // and the deck's real playhead when this row is live. -1 hides
            // the playhead entirely.
            [this, deckIdx, layerIdx] () -> int
            {
                if (preview.active.load (std::memory_order_relaxed))
                    return (int) preview.pos;
                const bool deckIsLive = transportRunning.load() && session.activeDeck() == deckIdx;
                if (! deckIsLive) return -1;
                return (int) session.decks[(size_t) deckIdx].phaseOf (layerIdx);
            });
        editorDeckIdx = deckIdx; editorLayerIdx = layerIdx;   // owner #3: Delete key target
        editorDockHeader.setTitle (juce::String (juce::CharPointer_UTF8 ("EDITOR \xe2\x80\x94 ")) + name);
        editorView.addAndMakeVisible (*clipEditorContent);

        libraryDockOpen = false;
        mixerDockOpen   = false;
        editorDockOpen  = true;
        refreshActiveView();
        resized();
        repaint();
    }

    // Reverse of openClipEditor()/openVoiceEditor() -- destroys whichever
    // content is hosted (ClipEditorContent's dtor stops any in-flight
    // preview) and closes the dock slot. Safe to call even when nothing is
    // open (every Library/Mixer-opening call site above calls this
    // unconditionally for mutual exclusion).
    void closeEditorDock()
    {
        if (! editorDockOpen && clipEditorContent == nullptr && voiceEditorContent == nullptr) return;
        clipEditorContent.reset();
        voiceEditorContent.reset();
        editorDeckIdx = editorLayerIdx = editorVoiceIdx = -1;   // owner #3: no Delete target while closed
        editorDockOpen = false;
        refreshActiveView();
    }

    // PerformLive UI/UX Design Notes (Studio One reference): "Double-clicking
    // a pad should open the Pad Editor INSIDE the bottom panel" -- the Pad/FX
    // counterpart of openClipEditor(), hosting VoiceEditorContent in the SAME
    // editorView dock slot instead of a second dock. Rebind Key/Replace
    // Audio/Clear Slot callbacks below are showVoiceSlotMenu()'s own former
    // logic, moved here verbatim (that popup menu is gone -- task #98).
    std::unique_ptr<juce::FileChooser> voiceEditorChooser;
    void openVoiceEditor (bool isPad, int idx)
    {
        auto& bank        = isPad ? padBank : fxBank;
        auto& panel        = isPad ? padPanel : fxPanel;
        auto& filePaths    = isPad ? padFilePaths : fxFilePaths;
        auto& nameOverride = isPad ? padNameOverride : fxNameOverride;
        auto& colourSet    = isPad ? padColourSet : fxColourSet;
        auto& colourArgb   = isPad ? padColourArgb : fxColourArgb;
        const auto action  = isPad ? ezaction::padAction (idx) : ezaction::fxAction (idx);

        auto& voice = bank.voices[(size_t) idx];
        if (! voice.loaded) return;

        const juce::String baseName = nameOverride[(size_t) idx].isNotEmpty()
                                           ? nameOverride[(size_t) idx]
                                           : juce::File (filePaths[(size_t) idx]).getFileNameWithoutExtension();
        const juce::String stats = (isPad ? juce::String ("Pad ") : juce::String ("FX ")) + juce::String (idx + 1)
                                    + juce::String (juce::CharPointer_UTF8 (" " "\xc2" "\xb7" " key ")) + keyForAction (action);

        clipEditorContent.reset();
        voiceEditorContent.reset();
        // Every operation is a thin call into the shared slot helpers above
        // (also used by the right-click context menu -- one implementation
        // each). onClose stays deferred via callAsync: it's invoked from
        // the editor's OWN Hide/Clear buttons, and a synchronous
        // closeEditorDock() would delete the component mid-click (the
        // owner's crash report).
        voiceEditorContent = std::make_unique<VoiceEditorContent> (
            baseName, stats, voice, nameOverride[(size_t) idx], colourSet[(size_t) idx], colourArgb[(size_t) idx],
            performlive::columnAccent (idx % 4),
            [this, isPad, idx] { promptRebindVoiceKey (isPad, idx); },
            [this, isPad, idx] { armVoiceMidiLearn (isPad, idx); },
            [this, isPad, idx] { promptReplaceVoiceAudio (isPad, idx); },
            [this, isPad, idx] { clearVoiceSlot (isPad, idx); },
            [this] { juce::MessageManager::callAsync ([this] { closeEditorDock(); resized(); }); },
            [this, &panel, idx] (const juce::String& newName) { panel.setSlotDisplayName (idx, newName); },
            [this, isPad, idx] (bool set, juce::uint32 argb) { setVoiceSlotColour (isPad, idx, set, argb); });

        editorVoiceIsPad = isPad; editorVoiceIdx = idx;   // owner #3: Delete key target
        editorDockHeader.setTitle (juce::String (juce::CharPointer_UTF8 ("EDITOR \xe2\x80\x94 ")) + baseName);
        editorView.addAndMakeVisible (*voiceEditorContent);

        libraryDockOpen = false;
        mixerDockOpen   = false;
        editorDockOpen  = true;
        refreshActiveView();
        resized();
        repaint();
    }

    //== clip editor preview (M3-T9, Loop Preview per roadmap Objective 1) ====
    // A single, separate, additively-mixed one-shot voice -- entirely
    // independent of session/Deck's own bar-quantized playhead/transport.
    // Plays the given layer's own audio from startSample through the end of
    // its raw buffer; loop=false stops there (the original M3-T9 behaviour,
    // an audition tool, not a performance one), loop=true wraps back to
    // startSample and keeps going instead, until stopPreview() is called.
    // Only one preview plays at a time; starting a new one replaces whatever
    // was previewing, including across different layers/decks.
    void startPreview (const ezdeck::Layer& previewLayer, int startSample, double rateRatio, bool loop)
    {
        preview.active = false;   // stop any in-flight preview before touching its state (see PreviewVoice's own comment)
        libraryPreview.active = false;   // roadmap "Sprint 2: Library" -- mutual exclusivity, see LibraryPreviewVoice's own comment
        preview.layer       = &previewLayer;
        preview.pos         = (double) juce::jlimit (0, juce::jmax (0, previewLayer.numFrames() - 1), startSample);
        preview.loopStart   = preview.pos;
        preview.rateRatio   = rateRatio;
        preview.loopEnabled = loop;
        preview.scrubTarget.store (-1.0, std::memory_order_relaxed);
        preview.active      = true;
    }

    // roadmap "scrubbing": a real-time-safe hand-off -- the message thread
    // only ever stores a REQUEST (atomic, no lock, no allocation); only the
    // audio thread (renderPreview(), top of its block) ever writes
    // preview.pos itself. Without this indirection, dragging while a
    // preview plays would have both threads writing the same non-atomic
    // double concurrently -- a genuine new race this avoids rather than
    // accepts.
    void scrubPreviewTo (int sampleIndex)
    {
        if (! preview.active.load (std::memory_order_relaxed)) return;
        preview.scrubTarget.store ((double) sampleIndex, std::memory_order_relaxed);
    }

    void stopPreview()
    {
        preview.active = false;

        // roadmap "Solo Preview": restore whatever mute state existed
        // before this preview soloed the layer, regardless of how the
        // preview ended (explicit stop, natural completion, editor closed).
        if (previewSoloActive)
        {
            auto& deck = session.decks[(size_t) previewSoloDeckIdx];
            for (int i = 0; i < ezdeck::kNumLayers; ++i)
                deck.layers[(size_t) i].enabled = previewSoloPrevEnabled[(size_t) i];
            previewSoloActive = false;
        }
    }

    // roadmap "Solo Preview": mutes every OTHER layer on this deck for the
    // duration of the preview, so auditioning one clip isn't stepped on by
    // the others -- restored by stopPreview() above regardless of how the
    // preview ends.
    void startPreviewSolo (int deckIdx, int layerIdx)
    {
        previewSoloActive  = true;
        previewSoloDeckIdx = deckIdx;
        auto& deck = session.decks[(size_t) deckIdx];
        for (int i = 0; i < ezdeck::kNumLayers; ++i)
        {
            previewSoloPrevEnabled[(size_t) i] = deck.layers[(size_t) i].enabled.load (std::memory_order_relaxed);
            if (i != layerIdx) deck.layers[(size_t) i].enabled = false;
        }
    }

    //== tap tempo (M4-T5, live re-warp wired in M4-T8) ========================
    // Updates masterTempo.bpm and, per M4-T8's objective, re-warps the
    // CURRENTLY ACTIVE deck specifically if LOCK is on (matching
    // project/MILESTONE_4_IMPLEMENTATION_PLAN.md M4-T8's exact wording --
    // TAP's cross-cutting re-warp effect is scoped to the active deck only,
    // unlike LOCK toggling or a per-deck override change, which affect
    // whichever deck(s) they directly resolve for). session.setTempo() is
    // called immediately regardless of LOCK, so bar-boundary math (already
    // recomputed fresh from session.getTempo() every render() call, per
    // Session.h) picks up the new tempo right away for deck-switch
    // quantization even when no re-warp follows.
    void handleTap()
    {
        tapTimestamps.push_back (juce::Time::getMillisecondCounterHiRes());
        if (tapTimestamps.size() > 32) tapTimestamps.erase (tapTimestamps.begin());

        const double bpm = computeTapTempo (tapTimestamps);
        if (bpm > 0.0)
        {
            masterTempo.bpm = juce::jlimit (20.0, 999.0, bpm);
            session.setTempo (masterTempo);
            if (tempoLockEnabled) reWarpDeckToEffectiveTempo (session.activeDeck());
        }
    }

    //== tempo lock + per-deck override (M4-T6, live re-warp wired in M4-T8) ===
    // Both manage/display state (see resolveEffectiveTempo above) AND, per
    // M4-T8's objective, re-warp the affected deck to match whenever that
    // state changes.
    //
    // Milestone 5 (M5-T5): also offers the stem/loop mode toggle and, in
    // stem mode, the 3 end-behavior choices. PRODUCT_REQUIREMENTS.md §3's
    // own rule -- "switching mode while a deck is playing stops it... rather
    // than silently changing behavior underneath live audio" -- is
    // implemented as: mode/end-behavior are only ever mutated on a deck
    // canMutateDeckState() agrees is safe right now. `Deck::mode` is a
    // plain, non-atomic field read every sample inside `Deck::render()`
    // (Deck.h, M5-T2); mutating it while the audio thread might be reading
    // it concurrently would be the same class of real, reachable data race
    // `project/KNOWN_BUGS.md` #13 already tracks for the clip editor's
    // fields, not a new one to add casually. Originally this gated on
    // `! session.isDeckAudible()` alone (inactive deck, no concurrent
    // reader -- ARCHITECTURE.md's resolved Architecture Decision Pending
    // #2/#8); Phase 1.1 P1 widened it to canMutateDeckState() once
    // getNextAudioBlock() started skipping renderPerTab() entirely while
    // stopped -- an ACTIVE deck is equally safe to mutate as long as
    // transportRunning is false, since nothing reads it either way. The
    // relevant menu items are simply disabled (matching this menu's own
    // existing "Clear deck tempo" convention) while mutation isn't safe,
    // and the handler re-checks again at click time (the menu is
    // asynchronous, so state could change between showing it and the click).
    void showDeckTempoMenu (int deckIdx)
    {
        auto& deck = session.decks[(size_t) deckIdx];
        const bool canChangeMode = canMutateDeckState (deckIdx);
        const bool isStem = (deck.mode == ezdeck::DeckMode::stem);

        juce::PopupMenu menu;
        // Owner #8: row colour as tappable boxes at the top -- no Color Row item.
        menu.addCustomItem (1000, std::make_unique<ColourSwatchRow> (
            rowColourSet[(size_t) deckIdx],
            [this, deckIdx] (juce::uint32 argb)
            {
                rowColourSet[(size_t) deckIdx]  = true;
                rowColourArgb[(size_t) deckIdx] = argb;
                refreshDeckPanelChrome(); repaint();
            },
            [this, deckIdx]
            {
                rowColourSet[(size_t) deckIdx] = false;
                refreshDeckPanelChrome(); repaint();
            }), nullptr);
        menu.addSeparator();
        menu.addItem (1, deckSourceBpm[(size_t) deckIdx].has_value()
                            ? "Tempo & time signature... (" + rowMeterName (deckIdx) + ", original " + juce::String (*deckSourceBpm[(size_t) deckIdx], 1) + " BPM)"
                            : "Tempo & time signature... (" + rowMeterName (deckIdx) + ", original not set)");
        menu.addItem (2, "Play as recorded (clear play-at tempo)", deckTempoOverride[(size_t) deckIdx].has_value());
        menu.addSeparator();
        menu.addItem (12, "Load stems into this row...", canChangeMode);
        menu.addItem (13, "Click & guide tracks...");
        // Owner #16: "there's no MIDI learn for the A1 A2 A3 -- we need that."
        menu.addItem (11, "MIDI Learn (trigger)...");
        menu.addSeparator();
        menu.addItem (3, isStem ? "Switch to loop mode" : "Switch to stem mode", canChangeMode);

        if (isStem)
        {
            menu.addSeparator();
            menu.addItem (4, "End behavior: Next",        canChangeMode, deck.stemEndBehavior == ezdeck::StemEndBehavior::next);
            menu.addItem (5, "End behavior: Next & Play", canChangeMode, deck.stemEndBehavior == ezdeck::StemEndBehavior::nextPlay);
            menu.addItem (6, "End behavior: Loop",        canChangeMode, deck.stemEndBehavior == ezdeck::StemEndBehavior::loop);
        }

        // Phase 1.1 P1 "Row Management" -- pure display metadata (name,
        // colour, notes, tags), none of it audio-thread-visible, so none of
        // these need canChangeMode's gate.
        menu.addSeparator();
        menu.addItem (7, "Rename Row...");
        menu.addItem (9, "Row Notes...");
        menu.addItem (10, "Row Tags...");

        menu.showMenuAsync (juce::PopupMenu::Options(), [this, deckIdx] (int result)
        {
            if (result == 0) return;

            auto& d = session.decks[(size_t) deckIdx];
            switch (result)
            {
                case 1: promptSetDeckTempo (deckIdx); return;
                case 2:
                    deckTempoOverride[(size_t) deckIdx].reset();
                    applyDeckSourceTempo (deckIdx);
                    reWarpDeckToEffectiveTempo (deckIdx);
                    showToast (deckLabel (deckIdx) + ": playing as recorded");   // Milestone 15: no silent state changes
                    repaint();
                    return;
                case 7: promptRenameRow (deckIdx); return;
                case 9: promptRowNotes (deckIdx); return;
                case 10: promptRowTags (deckIdx); return;
                case 12: promptLoadStemsIntoRow (deckIdx); return;
                case 13: showSongTrackMenu (deckIdx); return;
                case 11:
                {
                    // Owner #16: bind the next MIDI note/CC press to this
                    // ROW's trigger -- the same deckSlotAction the keyboard
                    // row-trigger already invokes.
                    const int sigIdx = signatureManager.signatureIndexForDeck (deckIdx);
                    const int slot   = deckIdx - signatureManager.firstDeckIndex (sigIdx);
                    midiRouter->armLearnButton (ezaction::deckSlotAction (slot));
                    showToast ("MIDI Learn: press a key/pad on your MIDI device to trigger " + deckLabel (deckIdx));
                    return;
                }
                default: break;
            }

            if (! canMutateDeckState (deckIdx)) return;   // re-checked at click time -- see this method's own comment

            switch (result)
            {
                case 3:
                    d.mode = (d.mode == ezdeck::DeckMode::stem) ? ezdeck::DeckMode::loop : ezdeck::DeckMode::stem;
                    showToast (deckLabel (deckIdx) + ": switched to " + (d.mode == ezdeck::DeckMode::stem ? "stem" : "loop") + " mode");
                    break;
                case 4: d.stemEndBehavior = ezdeck::StemEndBehavior::next;     showToast (deckLabel (deckIdx) + ": end behavior set to Next"); break;
                case 5: d.stemEndBehavior = ezdeck::StemEndBehavior::nextPlay; showToast (deckLabel (deckIdx) + ": end behavior set to Next & Play"); break;
                case 6: d.stemEndBehavior = ezdeck::StemEndBehavior::loop;     showToast (deckLabel (deckIdx) + ": end behavior set to Loop"); break;
                default: return;
            }
            repaint();
        });
    }

    // Phase 1.1 P1 "Rename Row": same display-override pattern as
    // promptRenameLayer() -- deckLabel() (above) is the one place that
    // reads it, so every existing caller picks up the rename for free.
    void promptRenameRow (int deckIdx)
    {
        auto* aw = new juce::AlertWindow ("Rename Row",
                                           "Enter a name for this row (e.g. \"Intro\", \"Verse\"), or clear the "
                                           "field to use the default \"" + defaultDeckLabel (deckIdx) + "\" label:",
                                           juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", rowName[(size_t) deckIdx]);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, deckIdx] (int result)
            {
                if (result == 1)
                {
                    rowName[(size_t) deckIdx] = aw->getTextEditorContents ("name").trim();
                    refreshSlotLabels();
                    showToast ("Row renamed to \"" + deckLabel (deckIdx) + "\"");
                    repaint();
                }
                delete aw;
            }), false);
    }

    void promptRowNotes (int deckIdx)
    {
        auto* aw = new juce::AlertWindow ("Row Notes -- " + deckLabel (deckIdx),
                                           "Free-text notes for this row:", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("notes", rowNotes[(size_t) deckIdx]);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, deckIdx] (int result)
            {
                if (result == 1)
                {
                    rowNotes[(size_t) deckIdx] = aw->getTextEditorContents ("notes");
                    showToast (deckLabel (deckIdx) + ": notes saved");
                }
                delete aw;
            }), false);
    }

    // Comma-separated, matching the brief's own "Tags" phrasing (a free-text
    // field, not a fixed taxonomy) -- same pattern the Library's own planned
    // metadata editing (Phase 1.1 Priority 2) will need, kept simple here
    // since Row Tags is this task's only concern.
    void promptRowTags (int deckIdx)
    {
        auto* aw = new juce::AlertWindow ("Row Tags -- " + deckLabel (deckIdx),
                                           "Comma-separated tags (e.g. \"energy, breakdown\"):",
                                           juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("tags", rowTags[(size_t) deckIdx]);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, deckIdx] (int result)
            {
                if (result == 1)
                {
                    rowTags[(size_t) deckIdx] = aw->getTextEditorContents ("tags").trim();
                    showToast (deckLabel (deckIdx) + ": tags saved");
                }
                delete aw;
            }), false);
    }

    // Async numeric entry -- same enterModalState/deleteWhenDismissed=false
    // pattern established in M3-T8's Set BPM dialog (reading the entered
    // value before explicitly deleting the AlertWindow at the end of the
    // callback, avoiding the use-after-free that deleteWhenDismissed=true
    // would cause by deleting the window before invoking the callback).
    // Owner: "I have to set the actual tempo first, and then when I want to
    // change the tempo the time-stretch comes in." Two fields: the song's
    // original tempo (a record of the recording -- it never changes the
    // audio) and an optional "play at" tempo, which is the only thing that
    // ever stretches the stems, all by one ratio.
    void promptSetDeckTempo (int deckIdx)
    {
        auto* aw = new juce::AlertWindow (deckLabel (deckIdx) + ": Tempo & time signature",
                                           "Original is the tempo the stems were recorded at - the app never guesses it. "
                                           "Play at stretches every stem together; leave it empty to play as recorded. "
                                           "Time signature is how many beats make a bar in this song.",
                                           juce::MessageBoxIconType::NoIcon);
        const auto& original = deckSourceBpm[(size_t) deckIdx];
        const auto& playAt   = deckTempoOverride[(size_t) deckIdx];
        aw->addTextEditor ("original", original.has_value() ? juce::String (*original, 1) : juce::String(), "Original tempo (BPM)");
        aw->addTextEditor ("playAt", playAt.has_value() ? juce::String (*playAt, 1) : juce::String(), "Play at (BPM) - optional");
        {
            juce::StringArray meters;
            meters.add ("Same as the " + juce::String (signatureManager.signature (signatureManager.signatureIndexForDeck (deckIdx)).name) + " group");
            for (const auto& m : ezstems::meterChoices()) meters.add (m.name);
            aw->addComboBox ("meter", meters, "Time signature");
            const int idx = rowMeter[(size_t) deckIdx].isEmpty() ? 0 : meters.indexOf (rowMeter[(size_t) deckIdx]);
            aw->getComboBoxComponent ("meter")->setSelectedItemIndex (juce::jmax (0, idx), juce::dontSendNotification);
        }
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        if (songClickTrack[(size_t) deckIdx] != nullptr) aw->addButton ("Read from click track", 2);
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, deckIdx] (int result)
            {
                const juce::String originalText = aw->getTextEditorContents ("original").trim();
                const juce::String playAtText   = aw->getTextEditorContents ("playAt").trim();
                const int meterIndex            = aw->getComboBoxComponent ("meter")->getSelectedItemIndex();
                const juce::String chosenMeter  = meterIndex <= 0 ? juce::String() : aw->getComboBoxComponent ("meter")->getText();
                delete aw;
                if (result == 1 && chosenMeter != rowMeter[(size_t) deckIdx]) setRowMeter (deckIdx, chosenMeter);
                if (result == 2) { readTempoFromRowClick (deckIdx); promptSetDeckTempo (deckIdx); return; }
                if (result != 1) return;

                const double o = originalText.getDoubleValue();
                if (o > 0.0)
                {
                    const double v = juce::jlimit (1.0, 999.0, o);
                    // the buffers are still at the old original's tempo only if
                    // they were never stretched; a corrected original just
                    // relabels them
                    if (deckBufferBpm[(size_t) deckIdx] > 0.0
                         && deckSourceBpm[(size_t) deckIdx].has_value()
                         && std::abs (deckBufferBpm[(size_t) deckIdx] - *deckSourceBpm[(size_t) deckIdx]) < 0.01)
                        deckBufferBpm[(size_t) deckIdx] = v;
                    deckSourceBpm[(size_t) deckIdx] = v;
                }
                else if (originalText.isEmpty() && ! playAtText.isEmpty())
                {
                    showToast (deckLabel (deckIdx) + ": enter the original tempo first - nothing was stretched");
                    return;
                }

                const double p = playAtText.getDoubleValue();
                if (playAtText.isEmpty() || p <= 0.0) deckTempoOverride[(size_t) deckIdx].reset();
                else deckTempoOverride[(size_t) deckIdx] = juce::jlimit (1.0, 999.0, p);

                applyDeckSourceTempo (deckIdx);
                reWarpDeckToEffectiveTempo (deckIdx);

                juce::String msg = deckLabel (deckIdx) + ": " + rowMeterName (deckIdx) + ", original " + juce::String (deckSourceBpm[(size_t) deckIdx].value_or (0.0), 1) + " BPM";
                if (deckTempoOverride[(size_t) deckIdx].has_value())
                    msg += ", playing at " + juce::String (*deckTempoOverride[(size_t) deckIdx], 1) + " BPM";
                else msg += ", playing as recorded";
                showToast (msg);
                repaint();
            }), false);
    }

    // UI_SPEC_PERFORM.md §2.2 (build-order step 4): the transport BPM
    // readout's "tap opens numeric entry" -- same async AlertWindow pattern
    // as promptSetDeckTempo just above, but sets the MASTER tempo directly
    // (session.setTempo(), already the single existing way masterTempo ever
    // changes -- handleTap()/recallScene() both go through it too) rather
    // than a per-deck override. No existing action covers "type in a master
    // BPM directly" -- this is a genuinely new entry point onto that
    // already-existing primitive, not a second implementation of one.
    void promptSetMasterTempo()
    {
        auto* aw = new juce::AlertWindow ("Set Master Tempo", "Enter a tempo (BPM):", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("bpm", juce::String (masterTempo.bpm, 1));
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw] (int result)
            {
                if (result == 1)
                {
                    const double v = aw->getTextEditorContents ("bpm").trim().getDoubleValue();
                    if (v > 0.0)
                    {
                        masterTempo.bpm = juce::jlimit (1.0, 999.0, v);
                        session.setTempo (masterTempo);
                        showToast ("Master tempo set to " + juce::String (masterTempo.bpm, 1) + " BPM");
                        repaint();
                    }
                }
                delete aw;
            }), false);
    }

    //== live re-warp integration (M4-T8) ======================================
    // Recomputes deckIdx's effective target tempo (resolveEffectiveTempo) and
    // re-warps every loaded layer in that deck to match it (ezdsp::reWarpLayer),
    // applying the result directly if the deck is inactive (per M4-T1's
    // resolved decision -- the audio thread never reads an inactive deck's
    // layers, so no swap primitive is needed there) or via Layer::
    // stagePendingSwap + Session::scheduleActiveDeckRewarp if it's the
    // currently active one. Calls only already-committed, already-tested
    // APIs from M4-T2/M4-T3/M4-T7 -- no new engine-level surface of its own.
    //
    // No-op if there is no forced target (resolveEffectiveTempo returns
    // std::nullopt, i.e. LOCK is off and this deck has no override --
    // "play as recorded"). Documented limitation, not solved here: once a
    // layer HAS been warped away from its native tempo, there is no cached
    // copy of its original, pre-warp audio anywhere in this app's data
    // model (Layer only ever holds ONE buffer, which M4-T2's swap
    // overwrites in place) -- so "play as recorded" can only mean "stop
    // forcing any further re-warp from this point on," not "restore the
    // exact original recording." Adding an original-buffer cache would be
    // new architecture beyond what M4-T1 established, out of this
    // integration-only task's scope.
    //
    // Milestone 5 (M5-T4): uses session.isDeckAudible(deckIdx), not
    // session.activeDeck() == deckIdx, to decide whether the swap primitive
    // is required -- the audio thread may already be reading a deck that
    // isn't (yet) "active" during a stem-to-stem crossfade (its
    // destination deck), per ARCHITECTURE.md's resolved Architecture
    // Decision Pending #8. Whether to *also* call scheduleActiveDeckRewarp()
    // is a separate question, answered by session.activeDeck() == deckIdx
    // specifically: that call always applies to whatever Session considers
    // the TRUE active deck (the crossfade's source, not its destination),
    // so it would target the wrong deck if deckIdx is only audible because
    // it's a crossfade's destination. In that narrow case the staged swap
    // deliberately waits, inert, until this deck becomes the true active
    // deck and some future re-warp trigger fires for it normally -- a
    // documented, narrow limitation (project/KNOWN_BUGS.md), not silently
    // worked around with new Session API surface.
    //
    // Owner: "don't play with the tempo until you are asked to." The stretch
    // is one ratio for the whole row: from the tempo the buffers are at now
    // (deckBufferBpm, or the user-set original) to the target. A row with no
    // original tempo set is never touched. With no target (no "play at", LOCK
    // off) a row that was stretched earlier is stretched back to its original.
    void reWarpDeckToEffectiveTempo (int deckIdx)
    {
        const auto& original = deckSourceBpm[(size_t) deckIdx];
        if (! original.has_value() || *original <= 0.0) return;

        auto target = resolveEffectiveTempo (deckTempoOverride[(size_t) deckIdx], tempoLockEnabled, masterTempo.bpm);
        if (! target.has_value()) target = *original;   // "play as recorded"

        const double from = deckBufferBpm[(size_t) deckIdx] > 0.0 ? deckBufferBpm[(size_t) deckIdx] : *original;
        if (std::abs (*target - from) < 0.01) return;   // already there -- don't re-process the audio for nothing

        auto& deck = session.decks[(size_t) deckIdx];
        const bool needsSwapPrimitive = session.isDeckAudible (deckIdx);
        const bool isTrueActiveDeck   = (session.activeDeck() == deckIdx);
        const bool stemMode = deck.mode == ezdeck::DeckMode::stem;
        bool anyStaged = false, allApplied = true;

        for (int l = 0; l < ezdeck::kNumLayers; ++l)
        {
            auto& layer = deck.layers[(size_t) l];
            if (! layer.loaded) continue;

            auto warped = ezdsp::reWarpLayer (layer.left, layer.right,
                                               deckFileSampleRate[(size_t) deckIdx],
                                               from, *target);
            if (warped.left.empty()) { allApplied = false; continue; }   // defensive -- reWarpLayer's own no-op-on-bad-input guard

            // a stem plays its whole length; only loop mode wants the fitted region
            const int region = stemMode ? 0 : warped.regionLength;

            if (needsSwapPrimitive)
            {
                // If a previously-staged swap for this layer hasn't been
                // applied yet, stagePendingSwap() returns false and this
                // request is dropped rather than overwriting an in-flight
                // buffer (M4-T1's concurrent-request policy) -- the next
                // triggering event will simply try again.
                if (layer.stagePendingSwap (std::move (warped.left), std::move (warped.right), region))
                {
                    taggedBpm[(size_t) deckIdx][(size_t) l] = *target;
                    anyStaged = true;
                }
                else allApplied = false;
            }
            else
            {
                layer.left         = std::move (warped.left);
                layer.right        = std::move (warped.right);
                layer.regionLength = region;
                taggedBpm[(size_t) deckIdx][(size_t) l] = *target;
            }
        }

        // the song's own click and guide stretch with the stems, so they stay on the beat
        if (allApplied && ! needsSwapPrimitive)
            for (bool isGuide : { false, true })
            {
                auto& slot = isGuide ? songGuideTrack[(size_t) deckIdx] : songClickTrack[(size_t) deckIdx];
                if (slot == nullptr) continue;
                auto warped = ezdsp::reWarpLayer (slot->mono, {}, slot->rate, from, *target);
                if (warped.left.empty()) continue;
                auto fresh = std::make_shared<SongTrack>();
                fresh->filePath = slot->filePath;
                fresh->rate = slot->rate;
                fresh->mono = std::move (warped.left);
                retiredSongTracks.push_back (slot);
                slot = fresh;
                (isGuide ? liveGuideTrack : liveClickTrack)[(size_t) deckIdx].store (fresh.get(), std::memory_order_release);
            }

        if (allApplied) deckBufferBpm[(size_t) deckIdx] = *target;
        if (isTrueActiveDeck && anyStaged) session.scheduleActiveDeckRewarp();
    }

    //== a song's own click and guide tracks =====================================

    bool decodeMono (const juce::File& file, std::vector<float>& mono, double& rate, juce::String& error)
    {
        if (ezlibrary::isRemotePath (file.getFullPathName())) { error = "not a local file"; return false; }
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr) { error = "could not open " + file.getFileName(); return false; }
        if (! eximport::readerGeometryIsSane (*reader, error)) return false;
        const int len = (int) reader->lengthInSamples;
        if (len <= 0) { error = file.getFileName() + " is empty"; return false; }
        mono.assign ((size_t) len, 0.0f);
        float* chans[1] { mono.data() };
        juce::AudioBuffer<float> dest (chans, 1, len);
        reader->read (&dest, 0, len, 0, true, false);
        rate = reader->sampleRate;
        return true;
    }

    /** Loads a click or guide track beside the row. quiet: no toast and the
        row's on/off switches are left alone (project load restores them). */
    bool loadSongTrack (int deck, bool isGuide, const juce::File& file, bool quiet = false)
    {
        if (deck < 0 || deck >= kNumDecks) return false;
        if (! canMutateDeckState (deck)) { showToast (deckLabel (deck) + ": stop this row first"); return false; }
        auto fresh = std::make_shared<SongTrack>();
        juce::String error;
        if (! decodeMono (file, fresh->mono, fresh->rate, error))
        {
            if (! quiet) showToast (juce::String (isGuide ? "Guide" : "Click") + " track: " + error);
            return false;
        }
        fresh->filePath = file.getFullPathName();

        auto& slot = isGuide ? songGuideTrack[(size_t) deck] : songClickTrack[(size_t) deck];
        auto& live = isGuide ? liveGuideTrack[(size_t) deck] : liveClickTrack[(size_t) deck];
        if (slot != nullptr) retiredSongTracks.push_back (slot);
        slot = fresh;
        live.store (fresh.get(), std::memory_order_release);

        if (! quiet)
        {
            // they brought it, so it plays: on, and the song's own
            (isGuide ? arrangements[(size_t) deck].guideCues : arrangements[(size_t) deck].guideClick) = true;
            (isGuide ? useSongGuide : useSongClick)[(size_t) deck] = true;
            showToast (deckLabel (deck) + ": " + (isGuide ? "guide" : "click") + " track " + file.getFileName());
        }
        rebuildGuideSchedule();
        refreshSlotLabels();
        return true;
    }

    void clearSongTrack (int deck, bool isGuide, bool quiet = false)
    {
        if (deck < 0 || deck >= kNumDecks) return;
        auto& slot = isGuide ? songGuideTrack[(size_t) deck] : songClickTrack[(size_t) deck];
        auto& live = isGuide ? liveGuideTrack[(size_t) deck] : liveClickTrack[(size_t) deck];
        live.store (nullptr, std::memory_order_release);
        if (slot != nullptr) { retiredSongTracks.push_back (slot); slot = nullptr; }
        if (! quiet) { rebuildGuideSchedule(); refreshSlotLabels(); showToast (deckLabel (deck) + ": " + (isGuide ? "guide" : "click") + " track removed"); }
    }

    /** Audio thread: adds the track to out, following the deck's playhead
        (deck file samples) at the deck's own speed. */
    void renderSongTrack (const SongTrack& t, float* outL, float* outR, int n, double deckPos, double deckFileRate) const
    {
        if (t.mono.size() < 2 || t.rate <= 0.0 || deckFileRate <= 0.0 || currentSampleRate <= 0.0) return;
        double idx = deckPos * (t.rate / deckFileRate);
        const double step = t.rate / currentSampleRate;
        const double last = (double) (t.mono.size() - 1);
        for (int i = 0; i < n; ++i, idx += step)
        {
            if (idx < 0.0 || idx >= last) continue;
            const size_t a = (size_t) idx;
            const float f = (float) (idx - (double) a);
            const float s = t.mono[a] + (t.mono[a + 1] - t.mono[a]) * f;
            outL[i] += s;
            outR[i] += s;
        }
    }

    ezcue::ClickReading readClickAudio (const std::vector<float>& mono, double rate) const
    {
        if (mono.empty() || rate <= 0.0) return {};
        // the first two minutes are plenty, and keep this quick
        const size_t n = juce::jmin (mono.size(), (size_t) (rate * 120.0));
        return ezcue::readClickTrack (mono.data(), n, rate);
    }

    ezcue::ClickReading readClickFile (const juce::File& file)
    {
        std::vector<float> mono; double rate = 0.0; juce::String error;
        if (! decodeMono (file, mono, rate, error)) return {};
        return readClickAudio (mono, rate);
    }

    /** Owner: "use that click from the stems to get the tempo" -- and the time
        signature, from the accented downbeat, when the click has one. */
    void readTempoFromRowClick (int deck)
    {
        const auto& t = songClickTrack[(size_t) deck];
        if (t == nullptr) { showToast (deckLabel (deck) + ": load a click track first"); return; }
        if (! canMutateDeckState (deck)) { showToast (deckLabel (deck) + ": stop this row first"); return; }
        const auto reading = readClickAudio (t->mono, t->rate);
        if (reading.bpm <= 0.0) { showToast (deckLabel (deck) + ": no steady clicks heard - type the tempo instead"); return; }
        // alternating clicks are eighth notes when the file's name says the song is half that
        const double named = ezcue::tempoFromName (t->filePath.toStdString());
        const bool eighths = reading.alternating && named > 0.0 && std::abs (named * 2.0 - reading.clicksPerMinute) < 1.0;
        const double bpm = eighths ? named : reading.bpm;
        if (deckBufferBpm[(size_t) deck] > 0.0 && deckSourceBpm[(size_t) deck].has_value()
             && std::abs (deckBufferBpm[(size_t) deck] - *deckSourceBpm[(size_t) deck]) < 0.01)
            deckBufferBpm[(size_t) deck] = bpm;
        deckSourceBpm[(size_t) deck] = bpm;

        juce::String meterNote;
        const auto meter = ezstems::meterForBeats (reading.beatsPerBar);
        if (meter.isNotEmpty() && setRowMeter (deck, meter, true)) meterNote = ", " + meter + " from its accented beat one";
        else if (reading.alternating)
            meterNote = juce::String (eighths ? " (eighth-note clicks, as the file name says" : " (the click alternates two sounds -- " + juce::String (reading.clicksPerMinute * 0.5, 0) + " BPM if they're eighth notes")
                      + "; no beat-one click, so check the time signature, now " + rowMeterName (deck) + ")";
        else meterNote = " (no beat-one accent -- check the time signature, now " + rowMeterName (deck) + ")";

        applyDeckSourceTempo (deck);
        reWarpDeckToEffectiveTempo (deck);
        rebuildGuideSchedule();
        showToast (deckLabel (deck) + ": original tempo " + juce::String (bpm, 1) + " BPM" + meterNote);
        repaint();
    }

    //== sections from a guide / cue track: speech recognition, then a review =====
    // Owner: "when it says bridge I see rap, or chorus I see chorus 3 -- work on
    // the speech to text so you don't write something wrong ... and I should be
    // able to make changes if the detection is wrong." The words are heard by
    // the operating system's recognizer (SpeechCues.h), listening only for
    // section names; matching the Motion Worship recordings is the fallback
    // where there is no recognizer. Nothing replaces the song's sections until
    // the review window's Apply.

    static std::vector<float> clip16k (const std::vector<float>& x, double rate, double startSec, double lenSec)
    {
        std::vector<float> out;
        if (x.size() < 2 || rate <= 0.0) return out;
        const double step = rate / 16000.0;
        const double a = juce::jmax (0.0, startSec * rate);
        const double b = juce::jmin ((double) x.size() - 1.0, (startSec + lenSec) * rate);
        out.reserve ((size_t) juce::jmax (0.0, (b - a) / step) + 1);
        for (double p = a; p < b; p += step)
        {
            const size_t i = (size_t) p;
            const float t = (float) (p - (double) i);
            out.push_back (x[i] + (x[i + 1] - x[i]) * t);
        }
        return out;
    }

    /** The recordings that name sections (matching fallback); counts are found by rhythm. */
    std::vector<ezcue::Reference> songFormReferences() const
    {
        std::vector<ezcue::Reference> refs;
        for (const auto& [stem, id] : cueIds)
        {
            const bool songForm = std::find_if (cueMenuNames.begin(), cueMenuNames.end(),
                                                [&stem] (const juce::String& nm) { return nm.startsWith ("Song Form/") && nm.endsWith ("/" + juce::String (stem)); })
                                  != cueMenuNames.end();
            if (! songForm || id < 0 || id >= (int) cueSources.size()) continue;
            const auto& src = cueSources[(size_t) id];
            refs.push_back ({ stem, ezcue::envelopeDb (src.mono.data(), src.mono.size(), src.rate) });
        }
        return refs;
    }

    std::vector<std::string> songFormStems() const
    {
        std::vector<std::string> stems;
        for (const auto& n : cueMenuNames)
            if (n.startsWith ("Song Form/")) stems.push_back (n.fromLastOccurrenceOf ("/", false, false).toStdString());
        return stems;
    }

    /** Any thread. The spoken bursts, heard by the recognizer when there is one. */
    static std::vector<ezcue::DraftSection> detectSections (const std::vector<float>& mono, double rate, const ezcue::DetectSettings& st,
                                                          int lengthBars, const std::vector<ezcue::Reference>& refs,
                                                          const std::vector<std::string>& stems)
    {
        const auto env    = ezcue::envelopeDb (mono.data(), mono.size(), rate);
        const auto bursts = ezcue::findBursts (env);
        if (bursts.empty()) return {};

        const auto table = ezcue::cuePhrases (stems);
        std::vector<ezspeech::Heard> heard;
        if (ezspeech::available())
        {
            std::vector<std::string> phrases;
            for (const auto& p : table) phrases.push_back (p.phrase);
            std::vector<std::vector<float>> clips;
            clips.reserve (bursts.size());
            for (const auto& b : bursts)
                clips.push_back (clip16k (mono, rate, b.startSeconds - 0.08, juce::jmin (4.0, b.lengthSeconds) + 0.2));
            std::string error;
            heard = ezspeech::recognise (clips, phrases, error);
            if (! error.empty()) juce::Logger::writeToLog ("Cue speech: " + juce::String (error) + " -- matching recordings instead");
        }
        juce::Logger::writeToLog ("Cue track: " + juce::String ((int) bursts.size()) + " bursts, "
                                  + (heard.empty() ? juce::String ("matched against the cue recordings") : juce::String ("heard by speech recognition")));

        std::function<ezcue::Match (size_t, const ezcue::Burst&)> nameBurst;
        if (! heard.empty())
            nameBurst = [&heard, &table] (size_t i, const ezcue::Burst&)
            {
                ezcue::Match m;
                if (i >= heard.size() || heard[i].text.empty()) return m;
                m.heard = heard[i].text;
                const auto name = ezcue::nameForPhrase (table, heard[i].text);
                if (name == "#count") { m.isCount = true; return m; }
                m.stem = name;
                // the engine's own "not sure" stays not sure in the review window
                m.score = heard[i].rejected ? juce::jmin (0.45f, 0.15f + heard[i].confidence * 0.3f)
                                            : juce::jmax (0.5f, heard[i].confidence);
                return m;
            };
        return ezcue::draftSections (bursts, env, refs, st, lengthBars, nameBurst);
    }

    /** Owner: "listen to the guide and get the sections of the song." */
    void buildSectionsFromRowGuide (int deck) { listenForSectionsOnDeck (deck, -1); }

    /** layer -1 = the row's own guide track, else that deck column. */
    void listenForSectionsOnDeck (int deck, int layer)
    {
        if (deck < 0 || deck >= kNumDecks) return;
        if (listeningForSections) { showToast ("Still listening to the last guide track -- one moment"); return; }

        auto audio = std::make_shared<std::vector<float>>();
        double rate = 0.0;
        juce::String label;
        if (layer < 0)
        {
            const auto t = songGuideTrack[(size_t) deck];
            if (t == nullptr) { showToast (deckLabel (deck) + ": load a guide track first"); return; }
            *audio = t->mono;
            rate = t->rate;
            label = "the guide track";
        }
        else
        {
            if (layer >= ezdeck::kNumLayers) return;
            const auto& lay = session.decks[(size_t) deck].layers[(size_t) layer];
            if (! lay.loaded || lay.left.empty()) return;
            *audio = lay.left;
            rate = deckFileSampleRate[(size_t) deck] > 0.0 ? deckFileSampleRate[(size_t) deck] : currentSampleRate;
            label = "\"" + layerDisplayName (deck, layer) + "\"";
        }
        if (! deckSourceBpm[(size_t) deck].has_value())
        {
            showToast (deckLabel (deck) + ": set the original tempo first (or read it from the click) so the cues land on bars");
            return;
        }
        ensureArrangementLength (deck);

        // bars in the audio's own tempo: stretched to "play at", or as recorded
        const double audioBpm = deckBufferBpm[(size_t) deck] > 0.0 ? deckBufferBpm[(size_t) deck] : *deckSourceBpm[(size_t) deck];
        ezcue::DetectSettings st;
        st.beatsPerBar   = juce::jmax (1, session.deckTempo (deck).beatsPerBar);
        st.secondsPerBar = (60.0 / audioBpm) * (double) st.beatsPerBar;
        st.leadBars      = arrangements[(size_t) deck].cueLeadBars;
        const int lengthBars = deckLengthBars (deck);
        auto refs  = std::make_shared<std::vector<ezcue::Reference>> (songFormReferences());
        auto stems = std::make_shared<std::vector<std::string>> (songFormStems());

        listeningForSections = true;
        showToast (deckLabel (deck) + ": listening to " + label + " for its cues...");
        juce::Component::SafePointer<SessionComponent> safe (this);
        juce::Thread::launch ([safe, deck, audio, rate, st, lengthBars, refs, stems, label]
        {
            auto drafts = std::make_shared<std::vector<ezcue::DraftSection>> (detectSections (*audio, rate, st, lengthBars, *refs, *stems));
            juce::MessageManager::callAsync ([safe, deck, audio, rate, drafts, label]
            {
                if (safe == nullptr) return;
                safe->listeningForSections = false;
                safe->openCueReview (deck, *drafts, audio, rate, label);
            });
        });
    }

    void openCueReview (int deck, const std::vector<ezcue::DraftSection>& drafts,
                        std::shared_ptr<std::vector<float>> audio, double rate, const juce::String& label)
    {
        if (drafts.empty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "No cues heard",
                "Nothing on " + label + " sounded like spoken cues.\n\nTry another track, or use Auto-section in PLAYBACK and name the sections.",
                "OK", this);
            return;
        }

        juce::StringArray names;
        for (const auto& p : ezcue::cuePhrases (songFormStems()))
            if (p.name != "#count") names.addIfNotAlreadyThere (juce::String (p.name));
        names.sortNatural();

        juce::Component::SafePointer<SessionComponent> safe (this);
        auto* content = new ezcuereview::CueReviewDialog (drafts, deckLabel (deck) + ": sections from " + label, names,
            [safe, audio, rate] (double start, double length)
            {
                // the spoken cue itself, a little either side, through the Library's audition voice
                if (safe == nullptr || audio == nullptr || rate <= 0.0) return;
                const size_t a = (size_t) juce::jmax (0.0, (start - 0.15) * rate);
                const size_t b = (size_t) juce::jmin ((double) audio->size(), (start + length + 0.35) * rate);
                if (b <= a) return;
                auto clip = std::make_shared<std::vector<float>> (audio->begin() + (long) a, audio->begin() + (long) b);
                safe->startLibraryPreview (clip, nullptr, safe->currentSampleRate > 0.0 ? rate / safe->currentSampleRate : 1.0);
            },
            [safe, deck] (const std::vector<ezcuereview::Choice>& chosen)
            {
                if (safe != nullptr) safe->applyReviewedSections (deck, chosen);
            });

        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Review sections";
        options.content.setOwned (content);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    void applyReviewedSections (int deck, const std::vector<ezcuereview::Choice>& chosen)
    {
        if (deck < 0 || deck >= kNumDecks || chosen.empty()) return;
        auto& a = arrangements[(size_t) deck];
        a.sections.clear();
        for (const auto& c : chosen)
        {
            ezarr::Section s;
            s.name = c.name.toStdString();
            s.startBar = c.startBar;
            s.colourArgb = ezplayback::colourForSectionName (s.name, (int) a.sections.size());
            a.addSection (s);
        }
        rebuildGuideSchedule();
        if (playbackView) playbackView->songChanged();
        showToast (deckLabel (deck) + ": " + juce::String ((int) chosen.size()) + " sections applied -- see PLAYBACK");
    }

    void toggleSongTrackPill (int deck, bool isGuide)
    {
        const bool has = (isGuide ? songGuideTrack : songClickTrack)[(size_t) deck] != nullptr;
        if (! has && isGuide && cueBank.empty()) { showSongTrackMenu (deck); return; }
        auto& a = arrangements[(size_t) deck];
        bool& flag = isGuide ? a.guideCues : a.guideClick;
        flag = ! flag;
        if (flag && ! has) (isGuide ? useSongGuide : useSongClick)[(size_t) deck] = false;   // nothing of the song's to use: the app's
        rebuildGuideSchedule();
        refreshSlotLabels();
        showToast (deckLabel (deck) + ": " + (isGuide ? "guide " : "click ") + (flag ? "on" : "off")
                   + (flag ? (has && (isGuide ? useSongGuide : useSongClick)[(size_t) deck] ? " (the song's own)" : " (built-in)") : ""));
    }

    void showSongTrackMenu (int deck)
    {
        const bool hasClick = songClickTrack[(size_t) deck] != nullptr;
        const bool hasGuide = songGuideTrack[(size_t) deck] != nullptr;
        const auto& a = arrangements[(size_t) deck];
        const bool canLoad = canMutateDeckState (deck);

        juce::PopupMenu m;
        m.addSectionHeader (deckLabel (deck) + "  -  CLICK");
        m.addItem (30, "Click on", true, a.guideClick);
        m.addItem (10, hasClick ? "Replace click track..." : "Load the song's click track...", canLoad);
        m.addItem (11, "Use the song's click (not the built-in one)", hasClick, hasClick && useSongClick[(size_t) deck]);
        m.addItem (12, "Read the original tempo from the click", hasClick);
        m.addItem (13, "Remove click track", hasClick && canLoad);
        m.addSeparator();
        m.addSectionHeader ("GUIDE");
        m.addItem (31, "Guide on", true, a.guideCues);
        m.addItem (20, hasGuide ? "Replace guide track..." : "Load the song's guide track...", canLoad);
        m.addItem (21, "Use the song's guide (not the built-in cues)", hasGuide, hasGuide && useSongGuide[(size_t) deck]);
        m.addItem (22, "Build the sections from the guide", hasGuide);
        m.addItem (23, "Remove guide track", hasGuide && canLoad);
        m.addSeparator();
        m.addItem (1, "Load stems into this row...", canLoad);

        const int sigIdx = signatureManager.signatureIndexForDeck (deck);
        const int slot   = deck - signatureManager.firstDeckIndex (sigIdx);
        auto options = juce::PopupMenu::Options();
        if (slot >= 0 && slot < guideCells.size() && guideCells[slot]->isShowing()) options = options.withTargetComponent (guideCells[slot]);

        m.showMenuAsync (options, [this, deck] (int r)
        {
            switch (r)
            {
                case 1:  promptLoadStemsIntoRow (deck); break;
                case 10: promptLoadSongTrack (deck, false); break;
                case 11: useSongClick[(size_t) deck] = ! useSongClick[(size_t) deck]; rebuildGuideSchedule(); refreshSlotLabels(); break;
                case 12: readTempoFromRowClick (deck); break;
                case 13: if (canMutateDeckState (deck)) clearSongTrack (deck, false); break;
                case 20: promptLoadSongTrack (deck, true); break;
                case 21: useSongGuide[(size_t) deck] = ! useSongGuide[(size_t) deck]; rebuildGuideSchedule(); refreshSlotLabels(); break;
                case 22: buildSectionsFromRowGuide (deck); break;
                case 23: if (canMutateDeckState (deck)) clearSongTrack (deck, true); break;
                case 30: toggleSongTrackPill (deck, false); break;
                case 31: toggleSongTrackPill (deck, true); break;
                default: break;
            }
        });
    }

    void promptLoadSongTrack (int deck, bool isGuide)
    {
        rowFileChooser = std::make_unique<juce::FileChooser> (juce::String ("Choose the song's ") + (isGuide ? "guide" : "click") + " track...",
                                                               juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        constexpr auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        rowFileChooser->launchAsync (flags, [this, deck, isGuide] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f != juce::File()) loadSongTrack (deck, isGuide, f);
        });
    }

    void promptLoadStemsIntoRow (int deck)
    {
        rowFileChooser = std::make_unique<juce::FileChooser> ("Choose the song's stems (the click and guide too, if it has them)...",
                                                               juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        constexpr auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems;
        rowFileChooser->launchAsync (flags, [this, deck] (const juce::FileChooser& fc)
        {
            auto files = fc.getResults();
            if (! files.isEmpty()) openStemImport (deck, files, {});
        });
    }

    // Owner: "when we are importing, a window where you can select this is
    // a click, this is a guide, read the names of the elements and put them
    // in the decks in a way that makes sense." StemImport.h.
    void openStemImport (int deck, juce::Array<juce::File> files, juce::StringArray assetIds)
    {
        if (! canMutateDeckState (deck)) { showToast (deckLabel (deck) + ": stop this row first to load into it"); return; }
        if (files.isEmpty()) return;

        // a tempo the files carry themselves (a DAW tag), else the row's own
        double hint = deckSourceBpm[(size_t) deck].value_or (0.0);
        if (hint <= 0.0)
            for (auto& f : files)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (f));
                if (reader == nullptr) continue;
                const double tag = embeddedTempoFrom (*reader);
                if (tag > 0.0) { hint = tag; break; }
            }

        if (hint <= 0.0)   // a tempo in the folder or file name: MultiTracks' "...-G-66.00bpm"
            for (auto& f : files)
                if (const double named = ezcue::tempoFromName (f.getFullPathName().toStdString()); named > 0.0) { hint = named; break; }

        auto plan = ezstems::planStemImport (files, assetIds, hint);
        plan.meter = rowMeter[(size_t) deck];
        juce::Component::SafePointer<SessionComponent> safe (this);
        auto* content = new ezstems::StemImportDialog (std::move (plan), deckLabel (deck),
            [safe] (const juce::File& click) { return safe != nullptr ? safe->readClickFile (click) : ezcue::ClickReading(); },
            [safe, deck] (const ezstems::Plan& p) { if (safe != nullptr) safe->applyStemImport (deck, p); });

        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Load stems";
        options.content.setOwned (content);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    void applyStemImport (int deck, const ezstems::Plan& plan)
    {
        if (! canMutateDeckState (deck)) { showToast (deckLabel (deck) + ": stop this row first to load into it"); return; }

        // the row starts clean: every deck, both song tracks
        for (int l = 0; l < ezdeck::kNumLayers; ++l) { clearLayer (deck, l); layerNameOverride[(size_t) deck][(size_t) l] = {}; layerAssetIds[(size_t) deck][(size_t) l] = {}; }
        clearSongTrack (deck, false, true);
        clearSongTrack (deck, true, true);
        deckRateSet[(size_t) deck] = false;
        deckBufferBpm[(size_t) deck] = 0.0;
        session.decks[(size_t) deck].mode = ezdeck::DeckMode::stem;
        if (plan.tempo > 0.0) deckSourceBpm[(size_t) deck] = plan.tempo;
        setRowMeter (deck, plan.meter, true);   // before the stems load: their bar counts use it

        int loaded = 0;
        bool click = false, guide = false;
        for (const auto& it : plan.items)
        {
            if (it.role >= 0 && it.role < ezdeck::kNumLayers)
            {
                loadLayer (deck, it.role, it.file);
                if (session.decks[(size_t) deck].layers[(size_t) it.role].loaded)
                {
                    ++loaded;
                    layerAssetIds[(size_t) deck][(size_t) it.role] = it.assetId;
                    layerNameOverride[(size_t) deck][(size_t) it.role] = it.name;
                }
            }
            else if (it.role == ezstems::kClick && ! click) click = loadSongTrack (deck, false, it.file, true);
            else if (it.role == ezstems::kGuide && ! guide) guide = loadSongTrack (deck, true, it.file, true);
        }
        if (click) { arrangements[(size_t) deck].guideClick = true; useSongClick[(size_t) deck] = true; }
        if (guide) { arrangements[(size_t) deck].guideCues  = true; useSongGuide[(size_t) deck] = true; }

        applyDeckSourceTempo (deck);
        rebuildGuideSchedule();
        refreshSlotLabels();
        if (playbackView) playbackView->songChanged();

        juce::String msg = deckLabel (deck) + ": " + juce::String (loaded) + (loaded == 1 ? " stem" : " stems");
        if (click) msg += " + click";
        if (guide) msg += " + guide";
        msg += plan.tempo > 0.0 ? ", original tempo " + juce::String (plan.tempo, 1) + " BPM" : ", tempo not set (playing as recorded)";
        showToast (msg);
        repaint();
    }

    //== a song's own time signature ============================================
    // Owner: "songs have different time signatures and the app should
    // accommodate that." A row plays in its signature group's meter unless the
    // song sets its own (rowMeter, one of ezstems::meterChoices()). Bars, cue
    // placement, counts, the built-in click, PLAYBACK and the stems' bar
    // lengths all read it through Deck::beatsPerBar (Session::effectiveTempo).

    int groupBeatsForDeck (int deck) const
    {
        return signatureManager.signature (signatureManager.signatureIndexForDeck (deck)).beatsPerBar;
    }

    juce::String rowMeterName (int deck) const
    {
        return rowMeter[(size_t) deck].isNotEmpty() ? rowMeter[(size_t) deck]
                                                    : juce::String (signatureManager.signature (signatureManager.signatureIndexForDeck (deck)).name);
    }

    /** meter "" = follow the row's group. quiet: no toast and nothing
        re-derived (project load, stem import -- the caller does that). */
    bool setRowMeter (int deck, const juce::String& meter, bool quiet = false)
    {
        if (deck < 0 || deck >= kNumDecks) return false;
        const int beats = meter.isEmpty() ? groupBeatsForDeck (deck) : ezstems::beatsForMeter (meter);
        if (beats <= 0) return false;
        if (! quiet && ! canMutateDeckState (deck)) { showToast (deckLabel (deck) + ": stop this row to change its time signature"); return false; }
        rowMeter[(size_t) deck] = meter;
        session.decks[(size_t) deck].beatsPerBar = beats;
        if (quiet) return true;
        applyDeckSourceTempo (deck);   // the stems' bar counts follow
        rebuildGuideSchedule();
        refreshSlotLabels();
        showToast (deckLabel (deck) + ": time signature " + rowMeterName (deck) + " (" + juce::String (beats) + " beats to the bar)");
        return true;
    }

    // The user set (or changed) a row's original tempo: every stem's tag and
    // bar count follow it, and the timeline/guide are told.
    void applyDeckSourceTempo (int deckIdx)
    {
        const double bpm = deckBpm (deckIdx);
        auto& deck = session.decks[(size_t) deckIdx];
        const int bpb = session.deckTempo (deckIdx).beatsPerBar > 0 ? session.deckTempo (deckIdx).beatsPerBar : 4;
        for (int l = 0; l < ezdeck::kNumLayers; ++l)
        {
            auto& layer = deck.layers[(size_t) l];
            if (! layer.loaded) continue;
            taggedBpm[(size_t) deckIdx][(size_t) l] = deckBufferBpm[(size_t) deckIdx] > 0.0 ? deckBufferBpm[(size_t) deckIdx] : bpm;
            if (deck.mode == ezdeck::DeckMode::stem && deckFileSampleRate[(size_t) deckIdx] > 0.0)
            {
                const double seconds = (double) layer.left.size() / deckFileSampleRate[(size_t) deckIdx];
                layer.stemBarLength = juce::jmax (1, (int) std::llround (seconds * bpm / 60.0 / bpb));
            }
        }
        ensureArrangementLength (deckIdx);
        if (currentSongDeck() == deckIdx) { adoptSongTempo (deckIdx); rebuildGuideSchedule(); }
        if (playbackView) playbackView->songChanged();
        refreshSlotLabels();
    }

    //== ui events =============================================================
    void buttonClicked (juce::Button* button) override
    {
        // Phase 1.1 P1: the signature rail's own Milestone 6 branch used to
        // live here (a buttonClicked() comparison loop) -- removed now that
        // SignatureRailButton isn't a juce::Button at all (see its own
        // class comment); it's wired via onTap at construction instead,
        // exactly like the note below already describes for DeckCard/
        // DeckTriggerCell.

        // UI_SPEC_PERFORM.md step 3: the old triggerButtons/toggleOwner
        // branches that used to live here are gone along with those members
        // -- DeckCard/DeckTriggerCell aren't juce::Button::Listener at all,
        // they push gestures out via onTap/onDoubleTap/onMenu/onTrigger,
        // wired directly at construction (see the deck-grid setup above).

        if (button == &tapButton) { handleTap(); return; }

        if (button == &lockToggle)
        {
            tempoLockEnabled = lockToggle.getToggleState();
            // LOCK is global -- re-resolve and re-warp every deck across
            // every signature; a deck with its own override is unaffected
            // by resolveEffectiveTempo regardless (override always wins),
            // and an unloaded deck's re-warp is a harmless no-op (no layers
            // to warp), not a second, different code path.
            for (int d = 0; d < kNumDecks; ++d) reWarpDeckToEffectiveTempo (d);
            repaint();
            return;
        }
    }

    void timerCallback() override
    {
        // roadmap "Sprint 5: reliability" -- periodic autosave, piggybacked
        // on this same 15Hz UI timer rather than a second juce::Timer (a
        // Component only gets one via plain `private juce::Timer`
        // inheritance; MultiTimer would be the alternative, but a tick
        // counter is simpler for a once-a-minute cadence). See
        // autosaveIfDue()'s own comment for why this is unconditional
        // (no dirty-tracking) rather than gated on "has anything changed."
        if (++autosaveTickCounter >= 15 * 60) { autosaveTickCounter = 0; autosaveIfDue(); }

        // Milestone 8: a lightweight "is this pad audible right now"
        // indicator -- isActive() is safe to read from the message thread
        // (OneShotVoice's own atomic-gate design), so this never races the
        // audio thread's own render() calls.
        for (int i = 0; i < 12; ++i)
        {
            padPanel.setSlotActive (i, padBank.voices[(size_t) i].isActive());
            fxPanel.setSlotActive (i, fxBank.voices[(size_t) i].isActive());
        }
        // Owner: playing-pad light animation -- a free-running ~0.8Hz breath
        // (pads play at native rate, so there's no beat to lock to; a slow
        // breath reads as "alive" without implying a tempo).
        voicePulsePhase = std::fmod (voicePulsePhase + (0.8 / 15.0), 1.0);
        padPanel.setPulsePhase (voicePulsePhase);
        fxPanel.setPulsePhase (voicePulsePhase);

        // Milestone 10: meter levels, refreshed at this same UI-timer rate --
        // Mixer::updateAndGetMeterLevel() is explicitly message-thread-only
        // (it owns the peak-hold decay state), matching that contract.
        for (int c = 0; c < ezdeck::kNumMixerChannels && c < mixerStrips.size(); ++c)
        {
            mixerStrips[c]->setMeterLevel (mixer.updateAndGetMeterLevel ((ezdeck::MixerChannel) c));
            if (c < kNumStrips)
                mixerStrips[c]->setFxState (true, stripOn[(size_t) c].load (std::memory_order_relaxed));
        }
        for (auto& s : instruments) s.collectRetired();   // PX-D: free swapped-out plugins once the audio thread let go

        // Diagnostics for the native click/cues, on demand: set
        // PERFORMLIVE_GUIDE_DEBUG=1 and read the session log.
        static const bool guideDebug = juce::SystemStats::getEnvironmentVariable ("PERFORMLIVE_GUIDE_DEBUG", "0") == "1";
        if (guideDebug && ++guideDebugTicks % 30 == 0)
            juce::Logger::writeToLog ("guide: running=" + juce::String ((int) transportRunning.load())
                                      + " flags=" + juce::String (guideFlags.load()) + " guideDeck=" + juce::String (guideDeck.load())
                                      + " activeDeck=" + juce::String (session.activeDeck()) + " songDeck=" + juce::String (currentSongDeck())
                                      + " spb=" + juce::String (guideSamplesPerBar.load(), 1)
                                      + " clickMuted=" + juce::String ((int) mixer.getChannelMute (ezdeck::MixerChannel::Metro))
                                      + " cuesMuted=" + juce::String ((int) mixer.getChannelMute (ezdeck::MixerChannel::Cues))
                                      + " clickPeak=" + juce::String (guideDebugPeak.load(), 3)
                                      + " sched=" + juce::String (currentSchedule != nullptr ? (int) currentSchedule->size() : -1)
                                      + " bank=" + juce::String ((int) cueBank.size()));

        serviceSectionPlayback();

        // Owner: per-track outputs are "essential for the app". The spec hid
        // the selector on a stereo device, which made the feature invisible
        // until the right driver happened to be chosen -- so it is always
        // shown now. On a device without that pair, choosing it is remembered
        // and toasted as "playing through Main until it is" (see
        // onOutputRouteChanged), and comes alive when the interface is picked.
        for (int c = 0; c < mixerStrips.size(); ++c)
            mixerStrips[c]->setOutputRouteControlVisible (true);

        // Milestone 11: highlight the active scene (PRD §11's "the active
        // scene is visually highlighted"). UI_SPEC_PERFORM.md §2.2: now
        // SceneButton's own "active" paint state (kIndigo fill + 2px border)
        // rather than a colourId swap -- same highlight logic, richer paint.
        for (int i = 0; i < sceneButtons.size(); ++i)
            sceneButtons[i]->setActiveState (i == activeSceneIndex);

        // Phase 1.1 P1: highlight the currently-viewed signature -- was a
        // plain buttonColourId swap on the old juce::TextButton array;
        // SignatureRailButton's own setSelected() now drives its full
        // glow/illuminate treatment instead (see that class's comment).
        for (int i = 0; i < signatureButtons.size(); ++i)
            signatureButtons[i]->setSelected (i == viewedSignature);

        // UI_SPEC_PERFORM.md §2.1: active = kIndigo fill; inactive =
        // transparent (was kCard in PX-001's own earlier pass).
        // Phase 1.1 P2: LIBRARY/MIXER highlight on their own dock-open flag,
        // not activeNavIndex -- they're toggled docks, not two of the
        // mutually-exclusive views the other indices still are.
        // Bug fix: this used to hardcode LIBRARY=index 2/MIXER=index 3 (true
        // in the original 5-item PERFORM/STEM EDITOR/LIBRARY/MIXER/STORE
        // array) -- removing the STEM EDITOR tab shifted kNavItems down to
        // PERFORM/LIBRARY/MIXER/STORE without updating these indices, so
        // STORE's tab (now at index 3) was lighting up on mixerDockOpen and
        // MIXER's own tab (now index 2) was lighting up on libraryDockOpen
        // instead -- caught live: opening the Mixer dock highlighted STORE.
        for (int i = 0; i < navButtons.size(); ++i)
        {
            // positions: 0 PERFORM, 1 PLAYBACK, 2 WEB, 3 STORE | 4 LIBRARY (dock), 5 MIXER (dock)
            static const int kNavIndexForPosition[] = { 0, kNavPlayback, kNavBrowser, 4, -1, -1 };
            const bool selected = (i == kNavLibraryPos) ? libraryDockOpen
                                : (i == kNavMixerPos)   ? mixerDockOpen
                                : (i < 6 && kNavIndexForPosition[i] == activeNavIndex);
            navButtons[i]->setColour (juce::TextButton::buttonColourId,
                                       selected ? juce::Colour (performlive::kIndigo)
                                                : juce::Colours::transparentBlack);
        }

        // PX-001: Output Volume passively mirrors the Master strip's own
        // authoritative gain (mixer.getMasterGain()) -- same read-only-
        // refresh pattern the meters above already use, so dragging one
        // slider is never fought by the other overwriting it mid-drag.
        if (! outputVolumeSlider.isMouseButtonDown())
            outputVolumeSlider.setValue (mixer.getMasterGain(), juce::dontSendNotification);

        // UI_SPEC_PERFORM.md §2.2: Play/Metronome reflect existing engine
        // state (read-only) -- same "one source of truth, many views"
        // pattern PX-001 established, now with the ONE Play button changing
        // its own glyph too (triangle/kPlay stopped, square/kDanger playing).
        {
            const bool playing = isActiveDeckPlaying();
            playButton.setButtonText (juce::String (juce::CharPointer_UTF8 (playing ? "\xe2\x96\xa0" : "\xe2\x96\xb6")));
            playButton.setColour (juce::TextButton::buttonColourId,
                                   juce::Colour (playing ? performlive::kDanger : performlive::kPlay));
        }
        metronomeButton.setColour (juce::TextButton::buttonColourId,
                                    metronomeEnabled ? juce::Colour (performlive::kPlay) : juce::Colour (performlive::kCard));
        bpmReadout.setBpm (masterTempo.bpm);   // TAP/scenes/promptSetMasterTempo() all change this live
        // UI_SPEC_MIXER.md §3.2: Metro strip's armed dot -- reusing this same
        // 15Hz timer, not a new one.
        mixerStrips[(int) ezdeck::MixerChannel::Metro]->setArmedIndicator (metronomeEnabled);

        // UI_SPEC_PERFORM.md §3/§6: DeckCard/DeckTriggerCell states, read
        // live from the SAME engine state paint()'s old text readout used to
        // print (session.activeDeck()/queuedDeck(), each layer's own
        // `enabled` atomic) -- no new engine coupling, just a different
        // presentation of state this app already tracked. Beat phase is
        // computed from session.masterPosition() (already read cross-thread
        // by the old status text, same risk profile, nothing new), not a
        // free-running animation, so a card that becomes visible mid-bar is
        // already in step. The queued pulse has no such requirement -- it's
        // just a free-running 2 Hz attention cue advanced once per tick, on
        // THIS existing 15 Hz timer (no second Timer anywhere).
        const int activeFlat = session.activeDeck();
        const int queuedFlat = session.queuedDeck();

        // SPEC_PERFORM_V2 GROUP H4: "when a linked loop plays, its linked
        // pad fires automatically." Edge-detected against
        // lastLinkedPadCheckFlat/Running so this fires exactly ONCE per
        // genuine transition into "now playing" for the active deck (a new
        // deck becoming active while running, OR transport starting for an
        // already-armed deck) -- never every tick, which would restutter
        // the exclusive pad bank 15 times a second. Pad-exclusivity itself
        // (padBank.exclusive) and deck/pad playback independence are both
        // pre-existing engine facts needing no new code here -- decks and
        // pads are already fully separate playback paths.
        {
            const bool running = transportRunning.load();
            if (running && (activeFlat != lastLinkedPadCheckFlat || ! lastLinkedPadCheckRunning))
            {
                for (int l = 0; l < ezdeck::kNumLayers; ++l)
                {
                    auto& layer = session.decks[(size_t) activeFlat].layers[(size_t) l];
                    const int pad = layerLinkedPad[(size_t) activeFlat][(size_t) l];
                    if (layer.loaded && layer.enabled.load() && pad >= 0 && pad < 12)
                        padBank.triggerVoice (pad);
                }
            }
            lastLinkedPadCheckFlat    = activeFlat;
            lastLinkedPadCheckRunning = running;
        }

        const double samplesPerBeat = masterTempo.bpm > 0.0 ? (currentSampleRate * 60.0 / masterTempo.bpm) : 0.0;
        double liveBeatPhase = 0.0;
        if (samplesPerBeat > 0.0)
        {
            const double posInBeat = std::fmod ((double) session.masterPosition(), samplesPerBeat);
            liveBeatPhase = posInBeat / samplesPerBeat;
        }

        // Phase 1.1 P1 "Scene Buttons... pulse during playback": only the
        // active scene, only while transportRunning (see getNextAudioBlock's
        // own comment) -- matching "during playback" literally rather than
        // pulsing a merely-selected-but-stopped scene.
        if (transportRunning.load() && activeSceneIndex >= 0 && activeSceneIndex < sceneButtons.size())
            sceneButtons[activeSceneIndex]->setPulsePhase (liveBeatPhase);

        // Phase 1.1 P7 "BPM pulse" -- same transportRunning-gated liveBeatPhase.
        bpmReadout.setPulsing (transportRunning.load());
        if (transportRunning.load()) bpmReadout.setPulsePhase (liveBeatPhase);

        queuedPulsePhase = std::fmod (queuedPulsePhase + (2.0 / 15.0), 1.0);   // free-running 2 Hz cue, 15 Hz tick

        for (int slot = 0; slot < kNumSlots; ++slot)
        {
            const int flat = flatDeckIndexForSlot (slot);
            const bool isActiveDeckThisSlot = (flat == activeFlat);

            for (int l = 0; l < ezdeck::kNumLayers; ++l)
            {
                auto& layer = session.decks[(size_t) flat].layers[(size_t) l];
                auto* card  = deckCards[(size_t) slot][l];

                // SPEC_PERFORM_V2 GROUP A: the three-state model is now
                // literally (deselected / selected+stopped / playing), and
                // it no longer depends on layer.enabled (mute) at all --
                // "decks only grey when deselected" means EVERY non-active
                // deck reads uniformly grey, regardless of any layer's own
                // mute state (mute keeps its audio effect; it just no
                // longer gets its own separate dim-vs-dimmer visual, which
                // was only ever shown on non-active decks before this
                // change anyway).
                if (! layer.loaded)
                    card->setState (DeckCard::State::empty);
                else if (isActiveDeckThisSlot && transportRunning.load())
                    card->setState (DeckCard::State::live);       // playing
                else if (isActiveDeckThisSlot)
                    card->setState (DeckCard::State::armed);      // selected + stopped
                else
                    card->setState (DeckCard::State::disabled);   // deselected

                // Bug report: tapping a layer tab (onTap above, toggling
                // layer.enabled) produced no visible change. See DeckCard::
                // setMuted()'s own comment -- this is independent of the
                // selection state set just above. Reset to full alpha for an
                // empty slot too, so a since-cleared card can't stay stuck
                // faded from whatever its last loaded mute state happened to be.
                card->setMuted (layer.loaded && ! layer.enabled.load (std::memory_order_relaxed));

                // Beat-phase pulsing requires the layer to be actually
                // audible right now (loaded + enabled + transport running) --
                // an active-but-stopped deck shows the static "armed" look
                // above, not a pulse implying sound that isn't happening.
                if (isActiveDeckThisSlot && layer.loaded && layer.enabled.load() && transportRunning.load())
                    card->setBeatPhase (liveBeatPhase);
            }

            // SPEC_PERFORM_V2 GROUP A: same three-state split for the
            // trigger cell itself -- "playing" now additionally requires
            // transportRunning, and a selected-but-stopped row gets the new
            // `armed` state (DeckTriggerCell, above) instead of jumping
            // straight to the "playing" look the moment it's merely active.
            auto* cell = triggerCells[(size_t) slot];
            if (flat == activeFlat)
                cell->setState (transportRunning.load() ? DeckTriggerCell::State::playing
                                                          : DeckTriggerCell::State::armed);
            else if (flat == queuedFlat) cell->setState (DeckTriggerCell::State::queued);
            else                          cell->setState (DeckTriggerCell::State::idle);
            cell->setPulsePhase (queuedPulsePhase);
        }

        // UI_SPEC_PERFORM.md §2.6: status bar's "CPU: 6%" -- a REAL reading
        // (juce::AudioDeviceManager::getCpuUsage(), the audio callback's own
        // load fraction), not a fabricated number. deviceManager is
        // inherited from AudioAppComponent.
        lastCpuUsagePercent = deviceManager.getCpuUsage() * 100.0;

        // UI_SPEC_ARRANGEMENT_M5.md §4.5: the live playhead -- READ here (on
        // this same 15Hz UI timer, never driven from audio) and cached for
        // paint() to draw. Only computed while the view is actually visible,
        // matching this timer's own established "only do the work that's
        // visible" pattern (meter/pad-state refresh above do the same).
        repaint();
    }

    //== audio ================================================================
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
    {
        // UI_SPEC_PERFORM.md §3/§6: cached for the live-card beat-pulse phase
        // calc in timerCallback() (message-thread only) -- not read from the
        // audio callback.
        currentSampleRate = sampleRate;

        session.prepare (sampleRate);
        for (int d = 0; d < kNumDecks; ++d)
            session.decks[(size_t) d].setRateRatio (
                deckFileSampleRate[(size_t) d] > 0.0 ? deckFileSampleRate[(size_t) d] / sampleRate : 1.0);
        scratchR.assign ((size_t) samplesPerBlockExpected, 0.0f);

        // Milestone 7: per-tab scratch for Session::renderPerTab(), plus
        // (ENGINEERING_PRINCIPLES.md's real-time-safety rule; KNOWN_BUGS.md
        // #6 already tracks the one pre-existing exception to this,
        // scratchR's own runtime-resize fallback above -- not repeated for
        // this new code).
        for (int t = 0; t < ezdeck::kNumLayers; ++t)
        {
            tabScratchL[(size_t) t].assign ((size_t) samplesPerBlockExpected, 0.0f);
            tabScratchR[(size_t) t].assign ((size_t) samplesPerBlockExpected, 0.0f);
        }
        for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
        {
            trackScratchL[(size_t) t].assign ((size_t) samplesPerBlockExpected, 0.0f);
            trackScratchR[(size_t) t].assign ((size_t) samplesPerBlockExpected, 0.0f);
        }
        mixerScratchCapacity = samplesPerBlockExpected;

        // Milestone 8: pads' own stop-envelope timing constants are
        // sample-rate-derived (OneShotVoice::prepare()), and their scratch
        // buffer is sized here, never inside getNextAudioBlock().
        padBank.prepare (sampleRate);
        padScratchL.assign ((size_t) samplesPerBlockExpected, 0.0f);
        padScratchR.assign ((size_t) samplesPerBlockExpected, 0.0f);

        // Milestone 9: same reasoning as Pads above, for FX.
        fxBank.prepare (sampleRate);
        fxScratchL.assign ((size_t) samplesPerBlockExpected, 0.0f);
        fxScratchR.assign ((size_t) samplesPerBlockExpected, 0.0f);

        // Milestone 10: same reasoning again, for the metronome.
        metro.prepare (sampleRate);
        metroScratchL.assign ((size_t) samplesPerBlockExpected, 0.0f);
        metroScratchR.assign ((size_t) samplesPerBlockExpected, 0.0f);

        // Guide.h: the click's envelope is in device samples; the cue
        // recordings are resampled to the device rate here (message thread,
        // audio stopped) so render() is a plain copy.
        {
            // PX-C: one scratch buffer per input channel the device has open.
            int inputs = 0;
            if (auto* dev = deviceManager.getCurrentAudioDevice()) inputs = dev->getActiveInputChannels().countNumberOfSetBits();
            liveInScratch.assign ((size_t) inputs, std::vector<float> ((size_t) samplesPerBlockExpected, 0.0f));
            liveInputCount.store (inputs, std::memory_order_relaxed);
        }
        songClick.prepare (sampleRate);
        for (auto& s : instruments) s.prepare (sampleRate, samplesPerBlockExpected);
        for (auto& strip : strips)
        {
            strip->releaseResources();
            strip->setPlayConfigDetails (2, 2, sampleRate, samplesPerBlockExpected);
            strip->prepareToPlay (sampleRate, samplesPerBlockExpected);
        }
        cueScratchL.assign ((size_t) samplesPerBlockExpected, 0.0f);
        cueScratchR.assign ((size_t) samplesPerBlockExpected, 0.0f);
        prepareCueBank (sampleRate);
        rebuildGuideSchedule();

        // SPEC_OUTPUT_ROUTING.md: sized HERE (message thread, prepareToPlay
        // is called again whenever the device changes), never resized
        // inside getNextAudioBlock() -- same real-time-safety discipline
        // every other scratch buffer above already follows. Only the
        // POINTERS these hold are refreshed per-block; the vectors
        // themselves keep whatever size pairCount() reports right now.
        {
            const int pairs = pairCount();
            outputPairPtrsL.assign ((size_t) pairs, nullptr);
            outputPairPtrsR.assign ((size_t) pairs, nullptr);
        }
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        // SPEC_OUTPUT_ROUTING.md: gather a write pointer for every pair
        // prepareToPlay() already sized outputPairPtrsL/R to -- channel
        // 2p/2p+1 of the device's own buffer is pair p's L/R. No
        // allocation: outputPairPtrsL/R's SIZE was fixed back in
        // prepareToPlay(); this loop only overwrites already-existing
        // slots with this block's own pointers.
        const int numChannels = info.buffer->getNumChannels();
        const int numPairs    = (int) outputPairPtrsL.size();

        // PX-C: JUCE's AudioSourcePlayer copies each open input channel into
        // the same-numbered channel of this buffer before we run, and the
        // mix below overwrites those channels -- so the inputs are kept
        // aside first. Same-block, so a mic or DI adds no latency beyond
        // the device's own.
        {
            const int nIn = juce::jmin (liveInputCount.load (std::memory_order_relaxed), numChannels, (int) liveInScratch.size());
            for (int c = 0; c < nIn; ++c)
            {
                auto& dst = liveInScratch[(size_t) c];
                const int n = juce::jmin (info.numSamples, (int) dst.size());
                const float* src = info.buffer->getReadPointer (c, info.startSample);
                std::copy (src, src + n, dst.begin());
                for (int i = n; i < (int) dst.size(); ++i) dst[(size_t) i] = 0.0f;
            }
        }

        for (int p = 0; p < numPairs; ++p)
        {
            const int lCh = p * 2, rCh = p * 2 + 1;
            outputPairPtrsL[(size_t) p] = (lCh < numChannels) ? info.buffer->getWritePointer (lCh, info.startSample) : nullptr;
            outputPairPtrsR[(size_t) p] = (rCh < numChannels) ? info.buffer->getWritePointer (rCh, info.startSample) : nullptr;
        }

        // Defensive fallback -- mixDown() must never receive a null
        // pointer. In normal operation this never triggers (numPairs comes
        // from the SAME device prepareToPlay() just measured, so every
        // lCh/rCh above should already be < numChannels); it only matters
        // for a mono device (numChannels < 2, the one case this file
        // already had a scratchR fallback for before multi-pair routing
        // existed) or the near-impossible device-changed-mid-flight window.
        // Reuses the SAME scratch buffer for every gap -- inaudible
        // (nothing reads it back), never a crash.
        if ((int) scratchR.size() < info.numSamples) scratchR.assign ((size_t) info.numSamples, 0.0f);
        for (int p = 0; p < numPairs; ++p)
        {
            if (outputPairPtrsL[(size_t) p] == nullptr) outputPairPtrsL[(size_t) p] = scratchR.data();
            if (outputPairPtrsR[(size_t) p] == nullptr) outputPairPtrsR[(size_t) p] = outputPairPtrsL[(size_t) p];
        }

        auto* outL = outputPairPtrsL[0];   // pair 0 == Main -- preview/library-audition audio always monitors here
        auto* outR = outputPairPtrsR[0];

        // Milestone 7: decks route through the Mixer's Tab1-4 channels
        // instead of session.render()'s flat masterGain -- session.render()
        // itself is no longer called by the real app but stays fully tested
        // (switchtest.cpp) for regression purposes. Defensive clamp (never a
        // resize) against the near-impossible case of a host requesting a
        // block larger than prepareToPlay's own samplesPerBlockExpected hint.
        const int numSamples = juce::jmin (info.numSamples, mixerScratchCapacity);

        // Milestone 10: captured BEFORE session.renderPerTab() advances it --
        // the metronome is driven by Session's own clock (masterPosition()),
        // never an independent one, so it stays perfectly locked to the same
        // clock deck-switching uses (Metronome.h's own header comment).
        const int64_t masterPosAtStart = session.masterPosition();

        std::array<float*, ezdeck::kNumLayers> tabPtrsL {}, tabPtrsR {};
        for (int t = 0; t < ezdeck::kNumLayers; ++t)
        {
            tabPtrsL[(size_t) t] = tabScratchL[(size_t) t].data();
            tabPtrsR[(size_t) t] = tabScratchR[(size_t) t].data();
        }

        // Phase 1.1 P1: "selecting a deck should only select it; playback
        // begins only when Play is pressed or A1-A4 is triggered." Session::
        // renderPerTab() unconditionally renders decks[active] every block
        // (it has no play/pause concept of its own) and, critically, is also
        // what ADVANCES the active deck's playhead and resolves a queued
        // switch/crossfade at the next bar boundary. So the gate has to be
        // "don't call it at all" while stopped, not "call it and discard the
        // result" -- the latter would let the active deck's position (and
        // any in-flight queued switch) silently keep advancing while
        // nobody's listening, so Play would resume from the wrong place.
        // Skipping the call also means no engine file needs to change: an
        // in-flight crossfade simply freezes wherever it was and resumes
        // correctly once transportRunning goes true again, same as every
        // other piece of Deck-internal state.
        // Guide.h: where the song's playhead stood before this block, so the
        // cues can fire for exactly the span this block plays.
        const int guideDeckNow = guideDeck.load (std::memory_order_relaxed);
        const double guideDeckPosBefore = (guideDeckNow >= 0 && guideDeckNow < kNumDecks)
                                        ? session.decks[(size_t) guideDeckNow].playheadPosition() : 0.0;
        const int64_t guideCountInBefore = session.countInSamplesLeft();
        juce::ignoreUnused (guideCountInBefore);

        if (transportRunning.load())
        {
            session.renderPerTab (tabPtrsL, tabPtrsR, numSamples);
        }
        else
        {
            for (int t = 0; t < ezdeck::kNumLayers; ++t)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    tabPtrsL[(size_t) t][i] = 0.0f;
                    tabPtrsR[(size_t) t][i] = 0.0f;
                }
            }
        }

        // Milestone 8/9/10: pads, FX, and the metronome all route through
        // the Mixer's Pads/Fx/Metro channels.
        padBank.render (padScratchL.data(), padScratchR.data(), numSamples);
        fxBank.render (fxScratchL.data(), fxScratchR.data(), numSamples);

        // Bug report: "the current metronome produces static/noise." Root
        // cause -- metro.render() used to run unconditionally, driven by
        // masterPosAtStart (Session's own clock). That clock only ADVANCES
        // inside session.renderPerTab(), which is itself skipped while
        // transportRunning is false (see that gate's own comment above) --
        // so while stopped, masterPosAtStart is the exact SAME frozen sample
        // position on every single audio callback. Metronome.h's own
        // boundary-crossing logic then re-detects "we just crossed a beat"
        // from that same frozen position on every callback (every few
        // milliseconds), retriggering the click's envelope from scratch
        // dozens of times a second -- which is exactly what a fast, garbled
        // click retrigger sounds like: buzzing static, not a clean periodic
        // tick. Gating on transportRunning (identical to the tab-decks gate
        // just above) removes the frozen-clock case entirely: no sound
        // while stopped, a real once-per-beat click while playing --
        // matching every other DAW/looper's own metronome behavior, not
        // just papering over the symptom.
        renderGuide (numSamples, masterPosAtStart, guideDeckPosBefore);

        // PX-D: the song clock as plugins want it, then each instrument column
        // renders in place of its file -- playing or stopped, keys must sound.
        {
            const auto tempo = session.getTempo();
            const double beatLen = tempo.bpm > 0.0 ? (60.0 / tempo.bpm) * currentSampleRate : 0.0;
            hostPlayHead.update (transportRunning.load(), tempo.bpm, tempo.beatsPerBar,
                                 beatLen > 0.0 ? (double) masterPosAtStart / beatLen : 0.0, masterPosAtStart);
        }

        // The live tracks, playing or stopped -- a mic or keys must work
        // between songs. Each renders into its own buffer (never a deck's):
        // an instrument, or the device input it was given, else silence.
        {
            const int nIn = juce::jmin (liveInputCount.load (std::memory_order_relaxed), (int) liveInScratch.size());
            for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
            {
                auto* dstL = trackScratchL[(size_t) t].data();
                auto* dstR = trackScratchR[(size_t) t].data();
                if (instruments[(size_t) t].hasInstrument())
                {
                    instruments[(size_t) t].render (dstL, dstR, numSamples);
                    continue;
                }
                for (int i = 0; i < numSamples; ++i) { dstL[i] = 0.0f; dstR[i] = 0.0f; }
                const int ch = trackInput[(size_t) t].load (std::memory_order_relaxed);
                if (ch < 0 || ch >= nIn) continue;
                const bool stereo = trackStereo[(size_t) t].load (std::memory_order_relaxed) && ch + 1 < nIn;
                const auto& inL = liveInScratch[(size_t) ch];
                const auto& inR = liveInScratch[(size_t) (stereo ? ch + 1 : ch)];
                const int n = juce::jmin (numSamples, (int) inL.size());
                for (int i = 0; i < n; ++i) { dstL[i] = inL[(size_t) i]; dstR[i] = inR[(size_t) i]; }
            }
        }

        // PERFORM LIVE on every deck and live track: in place, before the
        // mixer. It keeps running while stopped so reverb and delay tails
        // ring out.
        for (int i = 0; i < kNumStrips; ++i)
        {
            if (! stripOn[(size_t) i].load (std::memory_order_relaxed)) continue;
            const bool isDeck = i < ezdeck::kNumLayers;
            const size_t k = (size_t) (isDeck ? i : i - ezdeck::kNumLayers);
            float* chans[2] { isDeck ? tabPtrsL[k] : trackScratchL[k].data(), isDeck ? tabPtrsR[k] : trackScratchR[k].data() };
            juce::AudioBuffer<float> view (chans, 2, numSamples);
            stripMidi.clear();
            strips[(size_t) i]->processBlock (view, stripMidi);
        }

        // Filled by loop, not by an initialiser list. The list version named
        // seven pointers for a seven-channel mixer; when the mixer grew to
        // eleven the missing four were value-initialised to NULL rather than
        // being a compile error, and mixDown dereferenced them on the first
        // audio callback. Built this way the array cannot be short.
        std::array<const float*, ezdeck::kNumMixerChannels> mixInL {}, mixInR {};
        for (int t = 0; t < ezdeck::kNumLayers; ++t)
        {
            mixInL[(size_t) t] = tabScratchL[(size_t) t].data();
            mixInR[(size_t) t] = tabScratchR[(size_t) t].data();
        }
        for (int t = 0; t < ezdeck::kNumLiveTracks; ++t)
        {
            mixInL[(size_t) ((int) ezdeck::MixerChannel::Live1 + t)] = trackScratchL[(size_t) t].data();
            mixInR[(size_t) ((int) ezdeck::MixerChannel::Live1 + t)] = trackScratchR[(size_t) t].data();
        }
        mixInL[(size_t) ezdeck::MixerChannel::Pads]  = padScratchL.data();
        mixInR[(size_t) ezdeck::MixerChannel::Pads]  = padScratchR.data();
        mixInL[(size_t) ezdeck::MixerChannel::Fx]    = fxScratchL.data();
        mixInR[(size_t) ezdeck::MixerChannel::Fx]    = fxScratchR.data();
        mixInL[(size_t) ezdeck::MixerChannel::Metro] = metroScratchL.data();
        mixInR[(size_t) ezdeck::MixerChannel::Metro] = metroScratchR.data();
        mixInL[(size_t) ezdeck::MixerChannel::Cues]  = cueScratchL.data();
        mixInR[(size_t) ezdeck::MixerChannel::Cues]  = cueScratchR.data();
        mixer.mixDown (mixInL, mixInR, outputPairPtrsL, outputPairPtrsR, numSamples);
        renderPreview (outL, outR, numSamples);
        renderLibraryPreview (outL, outR, numSamples);

        // defensive tail, see clamp above -- every pair now, not just Main,
        // since a routed channel's own pair needs the same guarantee.
        for (int p = 0; p < numPairs; ++p)
            for (int i = numSamples; i < info.numSamples; ++i)
            {
                outputPairPtrsL[(size_t) p][i] = 0.0f;
                outputPairPtrsR[(size_t) p][i] = 0.0f;
            }
    }

    // Additively mixes the preview voice on top of whatever session.render()
    // just wrote (Deck::render's own masterGain default is 0.5f; matched here
    // so a preview sits at the same relative loudness as either deck).
    // Snapshots active/layer/pos/rateRatio into locals before the loop so a
    // concurrent startPreview()/stopPreview() call from the message thread
    // (e.g. retriggering Preview on a different layer) can't be observed as a
    // torn read mid-block -- this callback either uses the state as it stood
    // at the top of this block, or the next block sees the fully-updated
    // state, never a mix of both.
    void renderPreview (float* outL, float* outR, int numSamples)
    {
        if (! preview.active.load (std::memory_order_relaxed)) return;

        const ezdeck::Layer* previewLayer = preview.layer;
        if (previewLayer == nullptr || ! previewLayer->loaded) { preview.active = false; return; }

        const int    total     = previewLayer->numFrames();
        double       pos       = preview.pos;
        const double rateRatio = preview.rateRatio;
        bool         stillActive = true;

        const bool loop = preview.loopEnabled.load (std::memory_order_relaxed);

        // roadmap "scrubbing": consume a pending seek request, if any (see
        // scrubPreviewTo()'s own comment on why this hand-off exists).
        const double scrubReq = preview.scrubTarget.exchange (-1.0, std::memory_order_relaxed);
        if (scrubReq >= 0.0) pos = juce::jlimit (0.0, (double) juce::jmax (0, total - 1), scrubReq);

        for (int i = 0; i < numSamples; ++i)
        {
            if (pos >= (double) (total - 1))
            {
                if (loop) pos = preview.loopStart;   // roadmap "Loop Preview" -- wrap instead of stopping
                else { stillActive = false; break; }
            }

            const int   idx0 = (int) pos;
            const int   idx1 = juce::jmin (total - 1, idx0 + 1);
            const float frac = (float) (pos - idx0);

            const float l0 = previewLayer->left[(size_t) idx0];
            const float l1 = previewLayer->left[(size_t) idx1];
            const float l  = l0 + frac * (l1 - l0);

            float r;
            if (! previewLayer->right.empty())
            {
                const float r0 = previewLayer->right[(size_t) idx0];
                const float r1 = previewLayer->right[(size_t) idx1];
                r = r0 + frac * (r1 - r0);
            }
            else
            {
                r = l;
            }

            outL[i] += l * previewGain;
            outR[i] += r * previewGain;
            pos += rateRatio;
        }

        preview.pos = pos;
        if (! stillActive) preview.active = false;
    }

    // roadmap "Sprint 2: Library" — audition for a LIBRARY entry, which
    // isn't an ezdeck::Layer (it's a file on disk, resolved and decoded on
    // demand) -- so this is a SEPARATE voice from PreviewVoice above, not a
    // reuse of it. Mutually exclusive with the stem-editor preview in both
    // directions (see startPreview()'s own new first line, and this one's),
    // matching UI_SPEC_LIBRARY.md §3's "one at a time, globally exclusive
    // (matches the v20 single-preview rule)" -- one audible preview across
    // the whole app, regardless of which view started it.
    struct LibraryPreviewVoice
    {
        std::atomic<bool> active { false };
        std::shared_ptr<const std::vector<float>> left, right;   // owned decoded copy; right may be null (mono -- played to both channels)
        double pos       { 0.0 };
        double rateRatio { 1.0 };
    };
    LibraryPreviewVoice libraryPreview;

    // std::atomic_load/atomic_store on the shared_ptr fields below (rather
    // than plain assignment/copy) matter here specifically: this is a
    // genuinely NEW cross-thread hazard PreviewVoice's own Layer* pointer
    // never had (a raw pointer to a long-lived, externally-owned Layer has
    // no lifetime/refcount to race on). A plain concurrent read/write of the
    // SAME shared_ptr instance from two threads is undefined behaviour in
    // C++17 even though the control block's refcounting itself is
    // thread-safe -- atomic_load/atomic_store is exactly the sanctioned
    // pattern for this hand-off (both default to seq_cst, consistent with
    // the plain `active` bool's own default-seq_cst assignment/load below,
    // so "active observed true" on the audio thread implies the preceding
    // left/right stores are visible too).
    void startLibraryPreview (std::shared_ptr<const std::vector<float>> left,
                               std::shared_ptr<const std::vector<float>> right,
                               double rateRatio)
    {
        libraryPreview.active = false;
        stopPreview();   // mutual exclusivity: stop the stem-editor preview too (the full method, not a bare flag -- it also restores any Solo Preview mute state)
        std::atomic_store (&libraryPreview.left, std::move (left));
        std::atomic_store (&libraryPreview.right, std::move (right));
        libraryPreview.pos       = 0.0;
        libraryPreview.rateRatio = rateRatio;
        libraryPreview.active    = true;
    }

    void stopLibraryPreview() { libraryPreview.active = false; }

    bool isLibraryPreviewActive() const { return libraryPreview.active.load (std::memory_order_relaxed); }

    void renderLibraryPreview (float* outL, float* outR, int numSamples)
    {
        if (! libraryPreview.active.load (std::memory_order_relaxed)) return;

        auto left = std::atomic_load (&libraryPreview.left);   // local shared_ptr copy -- keeps the buffer alive for this block even if stop/replace races in from the message thread
        if (left == nullptr || left->empty()) { libraryPreview.active = false; return; }
        auto right = std::atomic_load (&libraryPreview.right);

        const int    total     = (int) left->size();
        double       pos       = libraryPreview.pos;
        const double rateRatio = libraryPreview.rateRatio;
        bool         stillActive = true;

        for (int i = 0; i < numSamples; ++i)
        {
            if (pos >= (double) (total - 1)) { stillActive = false; break; }

            const int   idx0 = (int) pos;
            const int   idx1 = juce::jmin (total - 1, idx0 + 1);
            const float frac = (float) (pos - idx0);

            const float l0 = (*left)[(size_t) idx0];
            const float l1 = (*left)[(size_t) idx1];
            const float l  = l0 + frac * (l1 - l0);

            float r;
            if (right != nullptr && ! right->empty())
            {
                const float r0 = (*right)[(size_t) idx0];
                const float r1 = (*right)[(size_t) idx1];
                r = r0 + frac * (r1 - r0);
            }
            else r = l;

            outL[i] += l * previewGain;
            outR[i] += r * previewGain;
            pos += rateRatio;
        }

        libraryPreview.pos = pos;
        if (! stillActive) libraryPreview.active = false;
    }

    void releaseResources() override {}

    //== ui ===================================================================
    // Visual-polish pass (LoopLab reference): the reference's body background
    // is a radial gradient plus two soft blurred colour blobs, not a flat
    // fill -- gives the whole app a sense of depth even where nothing else
    // is drawn. Rendered ONCE into a cached Image sized to the current
    // bounds (called from resized(), not paint()) -- same "compute once,
    // paint only reads" convention DeckCard's own peak cache already
    // established, since redoing a multi-gradient fill of the full window
    // every repaint would be wasteful for something that never changes
    // shape between resizes. ColourGradient's own radial falloff to fully
    // transparent stands in for a real gaussian blur (JUCE has no cheap one
    // for an arbitrary fill) -- the same "approximate a blur with a
    // gradient" idea SceneButton/DeckCard's own glow already uses at a much
    // smaller scale.
    void rebuildBackgroundImageIfNeeded()
    {
        const int w = juce::jmax (1, getWidth());
        const int h = juce::jmax (1, getHeight());
        if (backgroundImage.isValid() && backgroundImage.getWidth() == w && backgroundImage.getHeight() == h)
            return;

        backgroundImage = juce::Image (juce::Image::ARGB, w, h, true);
        juce::Graphics g (backgroundImage);

        juce::ColourGradient base (juce::Colour (0xff141426), (float) w * 0.5f, (float) h * -0.10f,
                                    juce::Colour (0xff0a0a12), (float) w * 0.5f, (float) h * 0.7f, true);
        g.setGradientFill (base);
        g.fillAll();

        auto blob = [&] (float cxFrac, float cyFrac, float radiusFrac, juce::Colour c)
        {
            const float cx = (float) w * cxFrac, cy = (float) h * cyFrac;
            const float r  = (float) juce::jmax (w, h) * radiusFrac;
            juce::ColourGradient grad (c, cx, cy, c.withAlpha (0.0f), cx + r, cy, true);
            g.setGradientFill (grad);
            g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
        };
        blob (0.14f, -0.05f, 0.42f, juce::Colour (0xff7c5cff).withAlpha (0.10f));   // kIndigo, top-left
        blob (0.90f, 1.05f,  0.40f, juce::Colour (0xffd946ef).withAlpha (0.07f));   // magenta, bottom-right
    }

    void paint (juce::Graphics& g) override
    {
        rebuildBackgroundImageIfNeeded();
        g.drawImageAt (backgroundImage, 0, 0);

        // UI_SPEC_PERFORM.md §2.1: one merged 56px header row -- nav tabs are
        // real components positioned in resized(), everything else here is
        // either static chrome (brand/border) or a display-only readout
        // (signature indicator, speaker icon) with no component of its own.
        // This layout MUST mirror resized()'s own removeFromLeft/Right
        // sequence exactly, or the drawn labels drift from the real controls
        // they annotate.
        auto full = getLocalBounds();

        auto headerArea = full.removeFromTop (kHeaderHeight);
        g.setColour (juce::Colour (performlive::kCard));
        g.fillRect (headerArea);
        g.setColour (juce::Colour (performlive::kBorder));
        g.fillRect (headerArea.removeFromBottom (1));

        auto headerContent = headerArea.reduced (16, 0);

        // SPEC_PERFORM_V2 GROUP E: "replace the text block with a logo
        // image the owner will supply; leave a correctly-sized placeholder
        // that swaps to the image when provided." logoImage is loaded once
        // in the constructor via searchFor("logo.png") -- when a real file
        // is dropped in next to the exe (or working directory / C:/EzPlay,
        // same 3-candidate search every other asset in this file uses), it
        // draws here automatically, no further code change needed.
        auto brandArea = headerContent.removeFromLeft (200);
        if (logoImage.isValid())
        {
            g.drawImage (logoImage, brandArea.reduced (0, 6).toFloat(),
                         juce::RectanglePlacement (juce::RectanglePlacement::centred));
        }
        else
        {
            // Placeholder: a dashed box the same size a real logo would
            // occupy, so the layout doesn't jump when the owner's file
            // arrives -- same dashed-outline idiom this file already uses
            // for "nothing loaded yet" (SceneButton's unfilled state,
            // DeckCard's empty state).
            auto box = brandArea.reduced (0, 6);
            juce::Path outline, dashed;
            outline.addRoundedRectangle (box.toFloat(), 4.0f);
            const float dashes[] { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (juce::Colour (performlive::kBorder));
            g.strokePath (dashed, juce::PathStrokeType (1.0f));

            // Only reachable if the embedded logo failed to decode: the
            // product name in text, never a "LOGO" placeholder.
            g.setColour (juce::Colour (performlive::kTextBright));
            g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)).withExtraKerningFactor (0.04f));
            g.drawText ("PERFORMLIVE", box, juce::Justification::centred, false);
        }

        // Mirror resized()'s right-to-left removals so these display-only
        // rects land exactly where the real components (gear/selector/
        // slider) were positioned.
        auto gearArea       = headerContent.removeFromRight (44);
        headerContent.removeFromRight (12);
        auto selectorArea   = headerContent.removeFromRight (160);
        headerContent.removeFromRight (12);
        auto volumeArea     = headerContent.removeFromRight (120);
        auto speakerArea    = headerContent.removeFromRight (28).reduced (4, 10);
        auto signatureArea  = headerContent.removeFromRight (70);
        juce::ignoreUnused (gearArea, selectorArea, volumeArea);

        // Speaker icon -- a hand-drawn trapezoid + sound-wave arcs rather
        // than a Unicode glyph, deliberately, after the U+2699 gear glyph
        // fell back to a dot-cluster earlier this sprint program.
        {
            g.setColour (juce::Colour (performlive::kTextDim));
            juce::Path speaker;
            const float x = (float) speakerArea.getX(), yMid = (float) speakerArea.getCentreY();
            speaker.startNewSubPath (x, yMid - 3.0f);
            speaker.lineTo (x + 5.0f, yMid - 3.0f);
            speaker.lineTo (x + 10.0f, yMid - 7.0f);
            speaker.lineTo (x + 10.0f, yMid + 7.0f);
            speaker.lineTo (x + 5.0f, yMid + 3.0f);
            speaker.lineTo (x, yMid + 3.0f);
            speaker.closeSubPath();
            g.fillPath (speaker);
            juce::Path waves;
            waves.addEllipse (x + 12.0f, yMid - 6.0f, 6.0f, 12.0f);
            g.strokePath (waves, juce::PathStrokeType (1.2f));
        }

        // Signature indicator -- display-only, per §2.1; the rail (left of
        // the deck grid) is the actual control that changes it.
        g.setColour (juce::Colour (performlive::kTextBright));
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawText (signatureManager.signature (viewedSignature).name,
                    signatureArea.removeFromTop (signatureArea.getHeight() / 2),
                    juce::Justification::centredLeft, false);
        g.setColour (juce::Colour (performlive::kTextFaint));
        // SPEC_PERFORM_V2 GROUP F (105): unified with BPM's own identical
        // "dim caption under a bold readout" role (was 8.0f vs BPM's 9.0f,
        // an accidental 1pt drift between two same-role captions -- both
        // sit in roomy strips, safe to converge), plus the same kerning
        // every other uppercase caption in this header now gets.
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::plain)).withExtraKerningFactor (0.12f));
        g.drawText ("SIGNATURE", signatureArea, juce::Justification::centredLeft, false);

        // Phase 1.1 P1 "Top Toolbar... control grouping... more premium":
        // thin vertical dividers between the header's control groups -- a
        // common toolbar grouping cue, purely decorative. Positioned from
        // the SAME rects resized() already computed above for its own
        // invisible mirror-math, so a divider can never drift out of sync
        // with the real control it separates.
        {
            g.setColour (juce::Colour (performlive::kBorder).withAlpha (0.7f));
            const int dividerTop = headerArea.getY() + 12, dividerH = headerArea.getHeight() - 24;
            for (int x : { signatureArea.getX() - 8, volumeArea.getX() - 8, selectorArea.getX() - 8, gearArea.getX() - 8 })
                g.fillRect (x, dividerTop, 1, dividerH);
        }

        // UI_SPEC_PERFORM.md §2/§7 (build-order step 7): gated on WHICH
        // VIEW is active. The perform-area backgrounds (scene bar, rail,
        // deck panel) moved to PerformContentView::paint() so they scroll
        // with the content (see its class comment) -- only the status bar,
        // pinned outside the scroll viewport, is still painted here.
        if (activeNavIndex == 0)
        {
            // §2.6 status bar -- real values throughout, never fabricated:
            // lastCpuUsagePercent comes from deviceManager's own reading
            // (see timerCallback()), masterTempo.bpm/viewedSignature/
            // projectSelector.getText() are the same live state everything
            // else in the UI already reads.
            g.setColour (juce::Colour (performlive::kShellBg));
            g.fillRect (statusBarArea);
            auto statusContent = statusBarArea.reduced (16, 0);

            // SPEC_STATUSBAR_MOJIBAKE.md: the raw "\xc2\xb7" byte literals
            // rendered as "Â·" (juce::String(const char*) isn't guaranteed
            // UTF-8 on Windows) -- wrapped in CharPointer_UTF8, the same
            // established fix already used elsewhere in this file (gear/
            // play glyphs, the arrangement toolbar's identical middle-dot).
            const juce::String kDot = juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
            juce::String statusLine = "Ready" + kDot + "Tempo: " + juce::String (masterTempo.bpm, 1) + " BPM" + kDot + "Time Sig: "
                                     + signatureManager.signature (viewedSignature).name
                                     + kDot + "Project: " + projectSelector.getText()
                                     + kDot + "CPU: " + juce::String (lastCpuUsagePercent, 1) + "%";
            g.setColour (juce::Colour (performlive::kTextDim));
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::plain)));
            auto meterArea = statusContent.removeFromRight (60).reduced (0, 9);
            g.drawText (statusLine, statusContent, juce::Justification::centredLeft, true);

            g.setColour (juce::Colour (performlive::kBorder));
            g.drawRect (meterArea, 1);
            auto meterFill = meterArea.reduced (1).withWidth (juce::roundToInt (
                (float) (meterArea.getWidth() - 2) * juce::jlimit (0.0, 1.0, lastCpuUsagePercent / 100.0)));
            g.setColour (juce::Colour (lastCpuUsagePercent > 80.0 ? performlive::kDanger : performlive::kPlay));
            g.fillRect (meterFill);
        }

        // Phase 1.1 P3: MIXER's own heading is now DockHeaderBar's own
        // "MIXER" title (Maximize/Close alongside it) -- see this dock's own
        // construction comment. The background fill that used to happen
        // here is now DockShellView's own paint() (see its class comment for
        // why) -- mixerView paints its own background as part of its own
        // component lifecycle now, nothing left to do in the parent.
    }

    // The PERFORM content's own minimum layout height -- the sum of every
    // fixed piece resized() carves out of performView, with deck rows at
    // their touch floor (88px for 4 rows, 44px for SHOW ALL's 8 -- the
    // same floors the row layout below uses). performView is never sized
    // shorter than this, so when a dock or a short window squeezes the
    // viewport below it, a scrollbar appears instead of content
    // overlapping or vanishing (owner bug report).
    int requiredPerformHeight() const
    {
        const int rowCount = showAllDeckRows ? 8 : 4;
        const int rowFloor = showAllDeckRows ? 44 : 88;
        return kTransportSceneHeight              // transport + scenes row
             + 24                                  // deckPanelInner's 12px top/bottom margins
             + 28 + 4 + 20 + 8                     // panel header row + gaps + column header row
             + rowCount * (rowFloor + 8) - 8       // deck rows at the touch floor
             + 162;                                // pads/FX strip (incl. its taller touch header)
    }

    // UI_SPEC_PERFORM.md §2 (build-order steps 4/5/6): the header now
    // includes the nav tabs inline (one 56px row, not PX-001's separate
    // header+nav rows), transport+scenes merge into one 64px row directly
    // under it, the rail shrinks to 88px, and a new 26px status bar sits at
    // the bottom. `full` below is everything under the header; all four
    // view containers get these same bounds, refreshActiveView() decides
    // which is visible.
    void resized() override
    {
        rebuildBackgroundImageIfNeeded();

        auto full = getLocalBounds();

        // ---- Header (§2.1), 56px, shared chrome across every view ----
        auto headerArea = full.removeFromTop (kHeaderHeight);
        auto headerContent = headerArea.reduced (16, 0);

        headerContent.removeFromLeft (200);   // brand block -- paint() draws it, no component

        settingsButton.setBounds (headerContent.removeFromRight (44).reduced (0, 6));
        headerContent.removeFromRight (12);
        projectSelector.setBounds (headerContent.removeFromRight (160).reduced (0, 12));
        headerContent.removeFromRight (12);
        outputVolumeSlider.setBounds (headerContent.removeFromRight (120).reduced (0, 14));
        headerContent.removeFromRight (28);   // room for the speaker icon paint() draws just left of the slider
        headerContent.removeFromRight (70);   // signature indicator -- paint()-only, display never a control

        // Nav tabs fill what's left between the brand block and the fixed-
        // width items above -- sized to their own text, not a fixed guess,
        // so "STEM EDITOR" (the longest label) isn't clipped.
        auto navArea = headerContent.reduced (0, 6);
        juce::Font navFont (juce::FontOptions (12.0f, juce::Font::bold));
        for (int i = 0; i < navButtons.size(); ++i)
        {
            if (i == kNavLibraryPos) navArea.removeFromLeft (kNavDockGroupGap);   // the views, a gap, then the docks
            const int w = juce::GlyphArrangement::getStringWidthInt (navFont, navButtons[i]->getButtonText()) + 36;
            navButtons[i]->setBounds (navArea.removeFromLeft (w));
        }

        // §2.6 status bar -- pinned to the window bottom, OUTSIDE the
        // PERFORM scroll viewport (a status bar that scrolls away with the
        // content wouldn't be a status bar). Carved before the dock slots,
        // so docks sit above it and it stays readable with a dock open.
        statusBarArea = full.removeFromBottom (kStatusBarHeight);

        // Phase 1.1 P2: when the Library dock is open (but not maximized),
        // it takes a bottom slice of the below-header area (default ~50%,
        // drag-adjustable); the four mutually-exclusive views above shrink
        // to share the REMAINING area with it, rather than Library
        // overlapping them -- "docked," not floating on top. Maximized is
        // the one exception, matching ordinary OS window "maximize"
        // convention: Library takes the ENTIRE below-header area and the
        // main view underneath keeps its own full (uncovered-by-carving)
        // bounds -- covered entirely rather than squeezed into a sliver it
        // was never designed to render inside (an earlier version of this
        // reserved ~8% for the main view at 0.92 maximized fraction, which
        // visibly clipped PERFORM's deck grid -- that view's layout code
        // has no "very short" fallback and was never asked to have one).
        // libraryDockBounds is empty ({}) when closed, matching every other
        // view's own paint()-only background rects' "empty rect = not
        // shown" convention.
        if (libraryDockOpen)
        {
            if (libraryDockMaximized)
            {
                libraryDockBounds = full;
            }
            else
            {
                // Owner bug report ("the three lines are there but can't be
                // adjusted -- too low, I can't see any files"): the dragged
                // fraction always wins now -- the old hard cap that froze
                // every dock at ~211px is gone. The dock simply CARVES its
                // height out of the below-header area; PERFORM keeps a
                // 160px minimum sliver above it and scrolls (performScroll)
                // to reach anything that no longer fits -- so a tall dock
                // can never make pads/FX unreachable.
                const int dockH = juce::jlimit (120, juce::jmax (120, full.getHeight() - 160),
                                                 juce::roundToInt ((float) full.getHeight() * libraryDockHeightFraction));
                libraryDockBounds = full.removeFromBottom (dockH);
            }
        }
        else
        {
            libraryDockBounds = {};
        }

        // Phase 1.1 P3: same carving for the Mixer dock -- mutually
        // exclusive with the Library dock above by construction (the nav
        // handlers never let both be open at once), so reducing the SAME
        // `full` here is safe; at most one of these two branches ever
        // actually shrinks it.
        if (mixerDockOpen)
        {
            if (mixerDockMaximized)
            {
                mixerDockBounds = full;
            }
            else
            {
                // Same carve behaviour as the Library dock just above --
                // see its comment for the owner bug this fixes.
                const int dockH = juce::jlimit (120, juce::jmax (120, full.getHeight() - 160),
                                                 juce::roundToInt ((float) full.getHeight() * mixerDockHeightFraction));
                mixerDockBounds = full.removeFromBottom (dockH);
            }
        }
        else
        {
            mixerDockBounds = {};
        }

        // Third dock slot (editor) -- mutually exclusive with Library/Mixer
        // by construction (see openClipEditor()/closeEditorDock() and every
        // Library/Mixer-opening call site above), so carving `full` here is
        // safe on the same "at most one branch actually shrinks it" basis.
        if (editorDockOpen)
        {
            if (editorDockMaximized)
            {
                editorDockBounds = full;
            }
            else
            {
                // Same carve behaviour as the Library dock above -- see
                // its comment for the owner bug this fixes.
                const int dockH = juce::jlimit (120, juce::jmax (120, full.getHeight() - 160),
                                                 juce::roundToInt ((float) full.getHeight() * editorDockHeightFraction));
                editorDockBounds = full.removeFromBottom (dockH);
            }
        }
        else
        {
            editorDockBounds = {};
        }

        // PERFORM lives in a viewport now (see PerformContentView's own
        // comment): the viewport gets whatever the docks left; the content
        // is at least requiredPerformHeight() tall, so a vertical scrollbar
        // appears exactly when (and only when) the layout no longer fits.
        performScroll.setBounds (full);
        {
            const int needed    = requiredPerformHeight();
            const bool scrolls  = needed > full.getHeight();
            const int contentW  = juce::jmax (200, full.getWidth() - (scrolls ? performScroll.getScrollBarThickness() : 0));
            const int contentH  = juce::jmax (needed, full.getHeight());
            performView.setSize (contentW, contentH);
        }
        libraryView.setBounds (libraryDockOpen ? libraryDockBounds : full);
        mixerView.setBounds (mixerDockOpen ? mixerDockBounds : full);
        editorView.setBounds (editorDockOpen ? editorDockBounds : full);
        storeView.setBounds (full);
        playbackHolder.setBounds (full);
        if (playbackView) playbackView->setBounds (playbackHolder.getLocalBounds());

        // Phase 1.1 P2/P3: re-assert z-order on EVERY resized(), not just
        // once when the dock is first toggled open (refreshActiveView()) --
        // this fixed a real bug caught during this session's own screenshot
        // verification: maximizing the Mixer dock (a resized()-only action,
        // no refreshActiveView() call) could leave PERFORM's own children
        // stacked above mixerView's gaps, bleeding through around the
        // channel strips. Idempotent and cheap, so doing it unconditionally
        // here is simpler and more robust than hunting down every call site
        // that changes a dock's size/state and remembering to re-front it.
        if (libraryDockOpen) libraryView.toFront (false);
        if (mixerDockOpen)   mixerView.toFront (false);
        if (editorDockOpen)  editorView.toFront (false);

        // Milestone 11: the toast overlay floats centered near the bottom of
        // the whole window, above everything else -- outside every view
        // container, so it isn't hidden no matter which one is active.
        toast.setBounds (getWidth() / 2 - 200, getHeight() - 60, 400, 40);

        // ---- MIXER view (UI_SPEC_MIXER.md §2/§7 step 2): 44px heading row
        // (MIXER label + METERS toggle), then one full-width row of all 8
        // strips -- each min(160, availableWidth/8 - gap) wide, 12px gap,
        // centred if narrower than the view. Falls back to two rows of four
        // only if the window is too narrow for 8 strips at a 96px floor. ----
        {
            auto mixerArea = mixerView.getLocalBounds();

            // Phase 1.1 P3 "Mixer: Open docked, allow maximize, like
            // Library" -- same 28px DockHeaderBar strip + Maximize/Close
            // Library's own dock uses, replacing the old plain 44px
            // paint()-only "MIXER" heading (removed from paint(), above).
            // Owner touch pass: taller 36px strip so Maximize/Close are
            // real ~28px touch targets, not 22px slivers.
            auto dockHeaderStrip = mixerArea.removeFromTop (36);
            mixerDockHeader.setBounds (dockHeaderStrip);
            mixerCloseButton.setBounds (dockHeaderStrip.removeFromRight (40).reduced (4));
            mixerMaximizeButton.setBounds (dockHeaderStrip.removeFromRight (92).reduced (4));

            // Bug report: meters are always on now, no toggle -- the 44px
            // heading row that used to hold the METERS button is reclaimed
            // for the strips themselves instead of standing empty.
            mixerArea = mixerArea.reduced (16, 12);

            // Owner: "the mixer should still be straight, with a scroll bar to
            // see the rest when we're not full screen." One row, never wrapped:
            // strips widen to fill a big window and stop at a minimum width,
            // after which the row scrolls sideways. Master is pinned right.
            constexpr int gap = 10, groupGap = 24, labelH = 22;
            constexpr int minStripW = 96, maxStripW = 150;

            juce::Array<juce::Component*> decks, live, returns;
            for (int c = 0; c < mixerStrips.size(); ++c)
                (c < ezdeck::kNumLayers ? decks : c < ezdeck::kNumLayers + ezdeck::kNumLiveTracks ? live : returns).add (mixerStrips[c]);
            if (webStrip != nullptr) returns.add (webStrip.get());

            juce::Rectangle<int> masterCol;
            if (masterStrip != nullptr)
            {
                masterCol = mixerArea.removeFromRight (minStripW + 10);
                mixerArea.removeFromRight (gap + 6);
            }

            const int nStrips = decks.size() + live.size() + returns.size();
            constexpr int nGroups = 3;
            const int availW = mixerArea.getWidth();
            const int fixedW = (nStrips - nGroups) * gap + (nGroups - 1) * groupGap;
            const int stripW = nStrips > 0 ? juce::jlimit (minStripW, maxStripW, (availW - fixedW) / nStrips) : minStripW;
            const int contentW = nStrips * stripW + fixedW;
            const bool scrolls = contentW > availW;

            mixerStripViewport.setBounds (mixerArea);
            const int holderH = mixerArea.getHeight() - (scrolls ? mixerStripViewport.getScrollBarThickness() + 4 : 0);
            mixerStripHolder.setSize (juce::jmax (contentW, availW), holderH);
            mixerStripHolder.groups.clear();

            int x = scrolls ? 0 : (availW - contentW) / 2;
            auto placeGroup = [&] (const juce::String& name, juce::Colour colour, const juce::Array<juce::Component*>& items)
            {
                if (items.isEmpty()) return;
                const int w = items.size() * stripW + (items.size() - 1) * gap;
                mixerStripHolder.groups.push_back ({ name, { x, 0, w, holderH }, colour });
                int sx = x;
                for (auto* comp : items) { comp->setBounds (sx, labelH, stripW, holderH - labelH); sx += stripW + gap; }
                x += w + groupGap;
            };
            placeGroup ("DECKS",   juce::Colour (performlive::kTextDim), decks);
            placeGroup ("LIVE",    juce::Colour (0xffff6b8a), live);
            placeGroup ("RETURNS", juce::Colour (performlive::kTextDim), returns);
            mixerStripHolder.repaint();

            if (masterStrip != nullptr)
                masterStrip->setBounds (masterCol.getX(), masterCol.getY() + labelH, masterCol.getWidth(), holderH - labelH);
        }

        // ---- LIBRARY: docked panel (Phase 1.1 P2) -- its own header strip
        // (drag-to-resize/Maximize/Close) on top, libraryTab filling
        // whatever's left. ---- STORE: still a real full-screen view. ----
        {
            auto dockLocal = libraryView.getLocalBounds();
            // Owner touch pass -- see the Mixer dock strip comment above.
            auto headerStrip = dockLocal.removeFromTop (36);
            libraryDockHeader.setBounds (headerStrip);
            libraryCloseButton.setBounds (headerStrip.removeFromRight (40).reduced (4));
            libraryMaximizeButton.setBounds (headerStrip.removeFromRight (92).reduced (4));
            libraryTab->setBounds (dockLocal);
        }
        creatorsTab->setBounds (storeView.getLocalBounds());
        if (storeShowcase) storeShowcase->setBounds (storeView.getLocalBounds());
        storeBackButton.setBounds (storeView.getLocalBounds().removeFromTop (44).removeFromRight (140).reduced (8, 6));

        // ---- EDITOR: docked panel (PerformLive UI/UX Design Notes: same
        // resize/maximize behaviour as Library/Mixer, never a floating
        // window). Its own header strip (drag-to-resize/Maximize/Close) on
        // top, the currently-open ClipEditorContent (if any) filling the
        // rest -- ClipEditorContent's own resized() already adapts to
        // whatever width/height it's given (fixed-height rows, waveform
        // takes the remainder). ----
        {
            auto dockLocal = editorView.getLocalBounds();
            // Owner touch pass -- see the Mixer dock strip comment above.
            auto headerStrip = dockLocal.removeFromTop (36);
            editorDockHeader.setBounds (headerStrip);
            editorCloseButton.setBounds (headerStrip.removeFromRight (40).reduced (4));
            editorMaximizeButton.setBounds (headerStrip.removeFromRight (92).reduced (4));
            if (clipEditorContent != nullptr)
                clipEditorContent->setBounds (dockLocal);
            else if (voiceEditorContent != nullptr)
                voiceEditorContent->setBounds (dockLocal);
        }

        // ---- PERFORM view: performView-LOCAL coordinates throughout.
        // Background rects are stored on performView itself (it paints its
        // own backgrounds now, so they scroll with the content -- see
        // PerformContentView's own comment). The status bar was carved from
        // `full` at the top of this function, outside the scroll.
        auto pFull = performView.getLocalBounds();

        // §2.2 Transport + Scenes -- ONE 64px row directly under the
        // header now, replacing PX-001's separate scene-bar row and the
        // buried bottom-of-window transport row.
        auto transportSceneRow = pFull.removeFromTop (kTransportSceneHeight);
        performView.sceneBarBgArea = transportSceneRow;

        // 420px is the spec's own component-width total (48+40+40+96+64+72+44
        // = 404) with no room left for gaps or the row's own inset -- widened
        // to fit both without shrinking any control below its specified size.
        auto transportGroup = transportSceneRow.removeFromLeft (462).reduced (8, 10);
        playButton.setBounds (transportGroup.removeFromLeft (48));
        transportGroup.removeFromLeft (6);
        prevDeckButton.setBounds (transportGroup.removeFromLeft (40).reduced (0, 4));
        transportGroup.removeFromLeft (4);
        nextDeckButton.setBounds (transportGroup.removeFromLeft (40).reduced (0, 4));
        transportGroup.removeFromLeft (8);
        bpmReadout.setBounds (transportGroup.removeFromLeft (96));
        transportGroup.removeFromLeft (8);
        tapButton.setBounds (transportGroup.removeFromLeft (64).reduced (0, 2));
        transportGroup.removeFromLeft (6);
        lockToggle.setBounds (transportGroup.removeFromLeft (72).reduced (0, 2));
        transportGroup.removeFromLeft (6);
        metronomeButton.setBounds (transportGroup.removeFromLeft (44).reduced (0, 4));

        auto sceneGroup = transportSceneRow.reduced (16, 10);
        performView.sceneLabelArea = sceneGroup.removeFromLeft (56);
        const int sceneCount = juce::jmax (1, sceneButtons.size());
        const int sceneW = juce::jmax (76, sceneGroup.getWidth() / sceneCount);
        for (int i = 0; i < sceneButtons.size(); ++i)
            sceneButtons[i]->setBounds (sceneGroup.removeFromLeft (sceneW).reduced (3));

        // §2.3 Signature rail -- 88px wide (was 170 in PX-001), plus the
        // spec's own "+" button below the 7 signature buttons.
        auto railAreaOuter = pFull.removeFromLeft (kRailWidth);
        performView.railBgArea = railAreaOuter;

        // the song-tracks column (click / guide per row), aligned with the rows below
        const auto guideColOuter = pFull.removeFromLeft (kGuideColWidth);
        performView.guideColBgArea = guideColOuter;
        auto railArea = railAreaOuter.reduced (8, 12);
        railArea.removeFromTop (24);   // clear the two-line "TIME SIGNATURE" label painted in paint()
        for (int i = 0; i < signatureButtons.size(); ++i)
        {
            signatureButtons[i]->setBounds (railArea.removeFromTop (44));
            railArea.removeFromTop (6);
        }

        // 162 (was 150): +12 for the taller touch-friendly page-button
        // header row inside VoiceBankPanel -- the pad cells keep their size.
        auto voiceBankArea = pFull.removeFromBottom (162).reduced (12, 8);
        padPanel.setBounds (voiceBankArea.removeFromLeft (voiceBankArea.getWidth() / 2).reduced (6));
        fxPanel.setBounds (voiceBankArea.reduced (6));

        // UI_SPEC_PERFORM.md §2.4/§3: the deck grid panel -- absorbs
        // everything freed by the transport/scene row moving up and the
        // rail shrinking (step 4's own "let the deck grid absorb the
        // reclaimed blank strip").
        performView.deckPanelArea = pFull;
        auto deckPanelInner = pFull.reduced (12, 12);

        auto panelHeaderRow = deckPanelInner.removeFromTop (28);
        deckPanelLabel.setBounds (panelHeaderRow.removeFromLeft (50));
        deckBankLabel.setBounds (panelHeaderRow.removeFromLeft (90));
        showAllRowsToggle.setBounds (panelHeaderRow.removeFromRight (170).reduced (2));
        bankBButton.setBounds (panelHeaderRow.removeFromRight (44).reduced (2));
        bankAButton.setBounds (panelHeaderRow.removeFromRight (44).reduced (2));
        panelHeaderRow.removeFromRight (14);
        pageNextButton.setBounds (panelHeaderRow.removeFromRight (34).reduced (2));
        pageDotsLabel .setBounds (panelHeaderRow.removeFromRight (34));
        pagePrevButton.setBounds (panelHeaderRow.removeFromRight (34).reduced (2));

        deckPanelInner.removeFromTop (4);

        auto colHeaderRow = deckPanelInner.removeFromTop (20);
        constexpr int triggerColW = 72;
        constexpr int colGap = 8;
        // Four columns are laid out, never eight: the page decides WHICH
        // four layers those columns address. Card size therefore does not
        // change when the layer count grows.
        const int colW = (colHeaderRow.getWidth() - triggerColW - kColsPerPage * colGap) / kColsPerPage;
        {
            auto colHeaderCopy = colHeaderRow;
            for (int c = 0; c < kNumCols; ++c)
                deckColumnHeaders[(size_t) c].setVisible (c < kColsPerPage);
            for (int c = 0; c < kColsPerPage; ++c)
            {
                deckColumnHeaders[(size_t) c].setBounds (colHeaderCopy.removeFromLeft (colW));
                colHeaderCopy.removeFromLeft (colGap);
            }
        }

        deckPanelInner.removeFromTop (8);

        // Rows: 4 for one bank, 8 for SHOW ALL (§2.4). Owner bug report
        // ("when I select SHOW ALL some rows get hidden below the pads and
        // FX with no way of accessing them -- the decks need to be shrunk
        // so all 8 show"): SHOW ALL now uses a smaller 44px touch floor so
        // all 8 rows genuinely fit above the pads/FX strip; 4-row mode
        // keeps the original 88px floor. Either way the floor can no
        // longer hide content: performView is at least
        // requiredPerformHeight() tall (which uses these same floors), so
        // anything past the visible area is reachable by scrolling.
        const int rowCount = showAllDeckRows ? 8 : 4;
        const int rowFloor = showAllDeckRows ? 44 : 88;
        const int cardH = juce::jmax (rowFloor, deckPanelInner.getHeight() / rowCount - 8);

        for (int slot = 0; slot < kNumSlots; ++slot)
        {
            const int row = showAllDeckRows ? slot : slot - deckBank * 4;
            const bool visible = (row >= 0 && row < rowCount);

            for (int l = 0; l < kNumCols; ++l)
                deckCards[(size_t) slot][l]->setVisible (visible && (l / kColsPerPage) == deckPage);
            triggerCells[(size_t) slot]->setVisible (visible);
            if (slot < guideCells.size()) guideCells[slot]->setVisible (visible);
            if (! visible) continue;

            auto rowArea = juce::Rectangle<int> (deckPanelInner.getX(),
                                                  deckPanelInner.getY() + row * (cardH + 8),
                                                  deckPanelInner.getWidth(), cardH);
            if (slot < guideCells.size())
                guideCells[slot]->setBounds (guideColOuter.getX() + 6, rowArea.getY(), guideColOuter.getWidth() - 10, cardH);
            for (int c = 0; c < kColsPerPage; ++c)
            {
                const int l = deckPage * kColsPerPage + c;
                deckCards[(size_t) slot][l]->setBounds (rowArea.removeFromLeft (colW));
                rowArea.removeFromLeft (colGap);
            }
            triggerCells[(size_t) slot]->setBounds (rowArea);
        }
    }

private:
    juce::AudioFormatManager  formatManager;
    ezdeck::Session<kNumDecks> session;
    ezdeck::SignatureManager  signatureManager;
    int                       viewedSignature { 0 };   // which signature's 8 decks the UI currently shows

    // The master tempo/time signature that defines bar length for quantized
    // deck switching. Change this to change the grid the app switches on.
    // Milestone 6: this is deliberately still Session-wide (bpm) -- only the
    // structural beat count varies per signature (Deck::beatsPerBar), per
    // ARCHITECTURE.md's resolved Architecture Decision Pending #1.
    ezdeck::Tempo masterTempo { 120.0, 4 };

    // UI_SPEC_PERFORM.md §2.4/§3 (build-order step 3): the real deck grid,
    // replacing the old text readout + LayerToggle/DeckTriggerButton pair
    // entirely. deckCards[slot][layer] and triggerCells[slot] are the SAME
    // kNumSlots persistent-widget pattern flatDeckIndexForSlot() already
    // established -- always all 8 slots x 4 layers exist; the A/B + SHOW ALL
    // toggles below only change which ones are positioned/visible, never
    // which flat deck a given [slot][layer] pair represents.
    static constexpr int kNumCols = ezdeck::kNumLayers;   // 4 -- one column per layer/tab
    std::array<juce::OwnedArray<DeckCard>, kNumSlots> deckCards;
    juce::OwnedArray<DeckTriggerCell> triggerCells;
    juce::OwnedArray<SignatureRailButton> signatureButtons;

    // Panel chrome (§2.4): "DECKS [ BANK A ]" header, A/B toggle, SHOW ALL
    // toggle, and the 4 "DECK N" column headers (coloured via columnAccent()).
    juce::Label deckPanelLabel, deckBankLabel;
    juce::TextButton bankAButton, bankBButton;
    // PX-B: layers 5-8 live on a second page of the deck grid, reached by
    // sliding left/right. Only the page changes -- the same eight decks and
    // the same bank are still on screen, so this is a horizontal move
    // through the LAYERS, not a different set of decks.
    juce::TextButton pagePrevButton, pageNextButton;
    juce::Label      pageDotsLabel;
    int              deckPage { 0 };            // 0 = layers 1-4, 1 = layers 5-8
    static constexpr int kColsPerPage = 4;
    static constexpr int kNumPages    = ezdeck::kNumLayers / kColsPerPage;
    juce::ToggleButton showAllRowsToggle;
    std::array<juce::Label, kNumCols> deckColumnHeaders;
    int  deckBank { 0 };             // 0 = A (slots 0-3), 1 = B (slots 4-7) -- ignored when showAllDeckRows
    bool showAllDeckRows { false };
    // Set in resized(), read by paint() for background fills. All THREE are
    // stored in SessionComponent-ABSOLUTE coordinates (performView's own
    // position added back in) even though performView's CHILDREN are
    // positioned in performView-LOCAL coordinates -- paint() draws in
    // SessionComponent's own space, not performView's, since performView is
    // a plain juce::Component with no paint() override of its own.
    // Only the status bar's rect lives here now -- it's pinned outside the
    // PERFORM scroll viewport; the scrolling background rects moved onto
    // PerformContentView itself (see its class comment).
    juce::Rectangle<int> statusBarArea;
    double queuedPulsePhase { 0.0 };      // shared 2 Hz cycle for every queued trigger cell, driven by the 15 Hz timer -- no per-cell Timer
    double voicePulsePhase  { 0.0 };      // owner: playing-pad breathing light, ~0.8 Hz, same 15 Hz timer
    double currentSampleRate { 44100.0 }; // set in prepareToPlay(); message-thread read only, for the live beat-pulse phase calc

    // Milestone 7 built the Mixer itself and Tab1-4's per-tab scratch for
    // Session::renderPerTab(); Milestone 10 completes the UI (Pads/Fx/Metro
    // strips + Master + meters). mixerStrips' indices match
    // ezdeck::MixerChannel's enum order exactly (Tab1..Metro).
    ezdeck::Mixer mixer;
    juce::OwnedArray<MixerChannelStrip> mixerStrips;
    std::unique_ptr<MixerChannelStrip> masterStrip;
    std::unique_ptr<MixerChannelStrip> webStrip;   // the browser's level/mute -- see its construction
    juce::Viewport   mixerStripViewport;           // one straight row that scrolls sideways
    MixerStripHolder mixerStripHolder;             // every strip but Master, under its group label
    float webGain { 1.0f };
    bool  webMute { false };

    void pushWebLevel()
    {
        if (browserTab) browserTab->setMediaLevel (juce::jlimit (0.0f, 1.0f, webGain), webMute);
    }
    // Phase 1.1 P3 "Output routing" -- persisted selection per channel (see
    // MixerChannelStrip's own comment for why only index 0/"Main" is real).
    std::array<int, ezdeck::kNumMixerChannels> channelOutputRoute {};
    std::array<std::vector<float>, ezdeck::kNumLayers> tabScratchL, tabScratchR;
    int mixerScratchCapacity { 0 };

    // Milestone 10: the metronome -- routes through the Mixer's Metro
    // channel (solo-exempt from Mixer's own construction).
    ezdeck::Metronome metro;
    std::vector<float> metroScratchL, metroScratchR;

    // Milestone 11: 8 scene slots + their UI buttons + a shared notification
    // surface (generalized further in Milestone 15).
    std::array<Scene, 8> scenes;
    juce::OwnedArray<SceneButton> sceneButtons;
    ToastOverlay toast;
    // Visual-polish pass (LoopLab reference): the setTooltip() calls sprinkled
    // through this constructor need exactly one juce::TooltipWindow attached
    // somewhere in this component's tree to actually render anything --
    // without one, setTooltip() is a harmless no-op. Standard JUCE pattern:
    // one instance, parented to `this`.
    juce::TooltipWindow tooltipWindow { this };
    int activeSceneIndex { -1 };

    // SPEC_PERFORM_V2 GROUP E: owner-assignable time-signature colour,
    // keyed by signature index (not by Scene -- signatures aren't scenes).
    // Sized via signatureManager.size() (7) at the point of use, same
    // literal-7 policy ProjectFile.h's own array uses for the same reason.
    std::array<bool, 7>         signatureColourSet {};
    std::array<juce::uint32, 7> signatureColourArgb {};

    // Milestone 8: 12 pads, exclusive by default (PRD §8's own stated
    // default -- the "One pad at a time" settings TOGGLE itself is
    // Milestone 13's job). Routes through the Mixer's Pads channel.
    ezdeck::VoiceBank<12> padBank;
    VoiceBankPanel padPanel { "PADS", 12 };
    std::vector<float> padScratchL, padScratchR;

    // Milestone 9: 12 FX slots -- the identical VoiceBank<12> type (PRD §9's
    // own "structurally identical to Pads"), non-exclusive. Routes through
    // the Mixer's Fx channel.
    ezdeck::VoiceBank<12> fxBank;
    VoiceBankPanel fxPanel { "FX", 12 };
    std::vector<float> fxScratchL, fxScratchR;

    // tap tempo + tempo lock/per-deck override (M4-T5/M4-T6) -- state and
    // display only; no re-warp is triggered by any of this yet (see each
    // method's own comment)
    juce::TextButton   tapButton;
    // UI_SPEC_PERFORM.md §2.2: TextButton, not ToggleButton -- setClickingTogglesState(true)
    // keeps the exact same getToggleState()/setToggleState() API every
    // existing call site already uses, while gaining buttonOnColourId
    // (a FILLED on-state) that plain ToggleButton doesn't have.
    juce::TextButton   lockToggle;
    IconGlyphButton    settingsButton { IconGlyphButton::Glyph::gear };   // Milestone 13 -- PX-001: now a gear icon, lives in the header; SPEC_PERFORM_V2 GROUP F (104): real vector gear, not a font glyph

    // PX-001 (PerformLive Application Shell): presentation-only additions.
    // Every one of these either calls an existing ActionRegistry action, an
    // existing Mixer getter/setter, or showToast -- no new engine state, no
    // new mutation path.
    // Bug report: the STEM EDITOR nav tab was removed entirely (see
    // refreshActiveView()'s own comment) -- activeNavIndex only ever takes
    // 0 (PERFORM) or 4 (STORE) now; LIBRARY/MIXER are docks, independent of
    // activeNavIndex (see their own nav onClick handlers). 1-3 are unused.
    juce::OwnedArray<juce::TextButton> navButtons;   // PERFORM/LIBRARY/MIXER/STORE
    int activeNavIndex { 0 };                        // 0=PERFORM 4=STORE (1-3 unused)
    // UI_SPEC_PERFORM.md §2.2: Play is now ONE button (glyph/fill swap
    // in-place, see timerCallback()) -- stopButton is gone, merged into it.
    juce::TextButton playButton, prevDeckButton, nextDeckButton;
    IconGlyphButton  metronomeButton { IconGlyphButton::Glyph::metronome };   // SPEC_PERFORM_V2 GROUP F (104): real vector metronome icon, replacing the plain "M" glyph
    BpmReadout bpmReadout;
    juce::Slider outputVolumeSlider;   // moved to the header this step (§2.1's "Master volume") -- see resized()
    juce::Image  logoImage;   // SPEC_PERFORM_V2 GROUP E -- see constructor/paint(); invalid (default) until the owner supplies logo.png
    juce::Image  backgroundImage;   // Visual-polish pass -- see rebuildBackgroundImageIfNeeded()'s own comment
    juce::ComboBox projectSelector;    // §2.1 -- single honest placeholder entry, no real multi-project system exists
    double lastCpuUsagePercent { 0.0 };   // §2.6 status bar -- real value from deviceManager, refreshed on the 15Hz timer

    // UI_SPEC_PERFORM.md §2/§7 (build-order step 7): full-screen view swap.
    // PERFORM (index 0) owns everything below the header/nav; MIXER
    // owns the relocated strips; LIBRARY/STORE are placeholders per this
    // step's own explicit allowance ("can be placeholder... the point of
    // this step is the swap mechanism"). Plain juce::Component containers, no
    // subclass needed -- children keep their existing setBounds() calls,
    // just relative to the container's own local origin instead of the
    // window's, and a container's setVisible(false) hides every child in
    // one call (matching the reference's "mutually exclusive containers").
    PerformContentView performView;   // scrollable PERFORM content -- see PerformContentView's own comment
    juce::Viewport     performScroll; // hosts performView; vertical scrollbar appears whenever a dock/short window squeezes PERFORM below requiredPerformHeight()
    juce::Component    storeView;
    DockShellView mixerView, libraryView, editorView;   // Phase 1.1 P2/P3 + SPEC_PERFORM_V2 GROUP: self-painting dock containers -- see DockShellView's own comment
    std::unique_ptr<creators::CreatorsTab> creatorsTab;       // how to become a creator -- see CreatorsTab.h
    std::unique_ptr<ezstore::StoreShowcase> storeShowcase;   // what STORE opens on
    juce::TextButton storeBackButton;

    enum class StorePage { showcase, creators };
    StorePage storePage { StorePage::showcase };

    void showStorePage (StorePage p)
    {
        storePage = p;
        if (storeShowcase) storeShowcase->setVisible (p == StorePage::showcase);
        if (creatorsTab) creatorsTab->setVisible (p == StorePage::creators);
        storeBackButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xb9 Store")));
        storeBackButton.setVisible (p != StorePage::showcase);
        storeBackButton.toFront (false);
    }
    bool saveHandledOnExit { false };   // the quit prompt saved, or the owner chose Don't Save
    bool quitPromptOpen { false };
    bool listeningForSections { false };   // a guide track is being heard on a worker thread
    int autosaveTickCounter { 0 };   // roadmap "Sprint 5: reliability" -- ticks at 15Hz; fires autosaveIfDue() once a minute
    juce::File currentProjectFile;   // Bug report: Save/Save As -- see projectFilePath()/saveProjectAs()'s own comments
    bool reconfiguringOutputs { false };   // re-entrancy guard -- see requestAllOutputChannels()

    // Project selector menu item IDs -- see refreshProjectMenu()'s own comment.
    static constexpr int kMenuIdCurrent    = 1;
    static constexpr int kMenuIdNew        = 2;
    static constexpr int kMenuIdOpen       = 3;
    static constexpr int kMenuIdSave       = 4;
    static constexpr int kMenuIdSaveAs     = 5;
    static constexpr int kMenuIdClose      = 6;
    static constexpr int kMenuIdNewDefault = 7;   // owner: "New from Default Pack" -- see defaultPackFilePath()
    static constexpr int kMenuIdRecentBase = 100;   // + index into recentProjectPaths()
    std::unique_ptr<MySamplesTab> libraryTab;   // UI_SPEC_LIBRARY.md -- the real LIBRARY view

    // Phase 1.1 P2 "Library: open docked at ~50% height by default (not
    // full-screen), allow maximize/restore/resize" -- see the LIBRARY nav
    // button's onClick and refreshActiveView()'s own comments for why this
    // is independent of activeNavIndex.
    bool  libraryDockOpen             { false };
    float libraryDockHeightFraction   { 0.5f };    // fraction of the below-header area, while open and NOT maximized
    bool  libraryDockMaximized        { false };   // true = take the whole below-header area (see resized()'s own comment)
    juce::Rectangle<int> libraryDockBounds;   // SessionComponent-local; set in resized(), read by nothing yet (reserved for future dock-chrome painting)
    DockHeaderBar    libraryDockHeader;
    juce::TextButton libraryMaximizeButton, libraryCloseButton;

    // Phase 1.1 P3 "Mixer: Open docked, allow maximize, like Library" --
    // same mechanism, same DockHeaderBar, same fields, one dock slot at the
    // bottom of the window shared between Library and Mixer (opening one
    // closes the other -- see the MIXER/LIBRARY nav buttons' own onClick).
    bool  mixerDockOpen           { false };
    float mixerDockHeightFraction { 0.5f };
    bool  mixerDockMaximized      { false };
    juce::Rectangle<int> mixerDockBounds;
    DockHeaderBar    mixerDockHeader;
    juce::TextButton mixerMaximizeButton, mixerCloseButton;

    // PerformLive UI/UX Design Notes (Studio One reference): "one reused
    // Optional Bottom Panel... shared between Mixer, Library, Pad Editor,
    // FX Editor -- only one visible at a time." Third dock slot, same
    // mechanism as Library/Mixer above, hosting whichever clip is currently
    // being edited (deck layer today; Pad/FX slots once 103b/103c land).
    // Mutually exclusive with Library/Mixer -- see openClipEditor(), the
    // LIBRARY/MIXER nav handlers, and every onSlotLibraryTap/onTap site that
    // opens Library/Mixer, all updated to close this dock too.
    bool  editorDockOpen           { false };
    float editorDockHeightFraction { 0.5f };
    bool  editorDockMaximized      { false };
    juce::Rectangle<int> editorDockBounds;
    DockHeaderBar    editorDockHeader;
    juce::TextButton editorMaximizeButton, editorCloseButton;
    std::unique_ptr<ClipEditorContent> clipEditorContent;   // owned here now instead of a DialogWindow's LaunchOptions::content
    std::unique_ptr<VoiceEditorContent> voiceEditorContent;   // 103c -- the Pad/FX counterpart, same dock slot, mutually exclusive with clipEditorContent

    // Owner #3 ("delete key removes sample on deck and pads"): which slot
    // the open editor is editing, so the Delete key knows what to clear.
    // -1 = no editor open (reset by closeEditorDock()).
    int  editorDeckIdx  { -1 }, editorLayerIdx { -1 };   // set by openClipEditor
    int  editorVoiceIdx { -1 };                          // set by openVoiceEditor
    bool editorVoiceIsPad { false };

    std::vector<double> tapTimestamps;
    bool tempoLockEnabled { false };

    // Phase 1.1 P1 "Tempo Rules" (verified, no code change needed): "row
    // tempo editable; deck tempo inherited from row; individual deck tempo
    // editing disabled" already describes this field exactly, once Row=deck
    // and (brief-sense) Deck=layer per layerDisplayName()'s own comment --
    // this has only ever been indexed per flat-deck-index (per row), never
    // per-layer, and showLayerContextMenu() (the new per-layer menu)
    // deliberately has no tempo item. The Stem Editor's taggedBpm half/
    // double/tap-to-set controls (ClipEditorContent, above) look like a
    // per-layer tempo edit but aren't one: taggedBpm is a CALIBRATION
    // reference (what tempo a layer's audio was actually recorded at, used
    // to compute that layer's warp ratio), not an independent playback
    // target -- reWarpDeckToEffectiveTempo() always re-derives every
    // layer's warp ratio from the SAME single row-level effective tempo
    // whenever a taggedBpm changes. Correcting a wrong tag (e.g. an
    // octave-detection error) can't make one layer audibly diverge in
    // tempo from its row; it only fixes which ratio gets it there.
    //
    // SPEC_PERFORM_V2 GROUP C (re-verified, no code change needed): grepped
    // every tempo-related control in Main.cpp after Groups A/B landed --
    // the only two tempo menu items anywhere are "Set deck tempo..."/
    // "Clear deck tempo" on showDeckTempoMenu() (row-level, DeckTriggerCell
    // right-click). showLayerContextMenu() (the per-slot menu Group B just
    // added "Set Colour..." to) still has no tempo item of any kind. The
    // three bullets above remain exactly true post-Group-A/B.
    std::array<std::optional<double>, kNumDecks> deckTempoOverride;

    // Owner: "all the stems have the same tempo... I have to set the actual
    // tempo first, and then when I want to change the tempo the time-stretch
    // comes in." One tempo per row/song, entered by the user (a DAW-written
    // tempo tag may pre-fill it; detection never does). Until it's set,
    // nothing in the row is ever stretched, whatever LOCK or "play at" say.
    std::array<std::optional<double>, kNumDecks> deckSourceBpm;
    // The tempo the row's buffers currently play at after stretching; 0 =
    // untouched, exactly as recorded. Every layer is always at the same one.
    std::array<double, kNumDecks> deckBufferBpm {};
    std::array<juce::String, kNumDecks> rowMeter;   // a song's own time signature; "" = its signature group's (setRowMeter)

    // Owner: a song's own click and guide tracks live beside the row, never
    // on one of its eight decks. Mono at the file's own rate; the audio
    // thread reads them through the live* pointers (see renderSongTrack),
    // which only ever change while the row can't be heard. A replaced track
    // is kept in retiredSongTracks until the transport stops.
public:   // declared public above (the forward declaration); Clang refuses a change of access
    struct SongTrack { juce::String filePath; std::vector<float> mono; double rate { 0.0 }; };
private:
    std::array<std::shared_ptr<SongTrack>, kNumDecks> songClickTrack, songGuideTrack;
    std::array<std::atomic<SongTrack*>, kNumDecks> liveClickTrack, liveGuideTrack;
    std::vector<std::shared_ptr<SongTrack>> retiredSongTracks;
    std::array<bool, kNumDecks> useSongClick {}, useSongGuide {};   // set true per row in the constructor
    std::atomic<int>    guideSongTracks { 0 };       // bit 0: click from the song's track, bit 1: guide from it
    std::atomic<double> guideDeckFileRate { 0.0 };   // the guide deck's file rate, for the tracks' positions
    int guideRowOverride { -1 };                     // PERFORM: the last triggered row gets the guide, setlist or not
    std::unique_ptr<juce::FileChooser> rowFileChooser;
    juce::OwnedArray<GuideSlotCell> guideCells;

    //==========================================================================
    //  Section playback -- PlaybackHost implementation + the per-tick
    //  scheduler. Everything a performer does on the PLAYBACK screen lands
    //  here and is translated into one of Session's three sample-exact
    //  primitives (queueSeek / setSectionLoop / armStopAt) or a transport
    //  change. Bars live up here; samples live in the engine.
    //==========================================================================
    static constexpr int kNavPlayback = 5;
    // nav bar positions (kNavItems order): PERFORM, PLAYBACK, WEB, STORE, then the docks
    static constexpr int kNavLibraryPos   = 4;
    static constexpr int kNavMixerPos     = 5;
    static constexpr int kNavDockGroupGap = 36;   // the space between the views and the docks
    static constexpr int kNavBrowser  = 6;

    std::array<ezarr::Arrangement, kNumDecks> arrangements;
    std::vector<int> setlist;          // flat deck indices, in performance order
    int  setlistPos { -1 };            // index into setlist; -1 = nothing cued
    int  jumpModeValue { 1 };          // 0 next bar, 1 end of section, 2 now
    int  queuedSectionIdx { -1 };
    int  lastSectionIdx { -1 };
    bool countInActive { false };
    bool countInMetroWasMuted { false };

    juce::Component playbackHolder;
    std::unique_ptr<ezplayback::PlaybackView> playbackView;

    // ---- PERFORM LIVE channel strip on every deck and live track ----
    // Built into the app rather than loaded as a plugin. Strip index == mixer
    // channel index: 0-7 the decks, 8-11 LIVE 1-4. Created up front (cheap,
    // silent while off) so switching one on is never an allocation on stage.
    // Owner: "FX on every track, off by default."
    static constexpr int kNumStrips = ezdeck::kNumLayers + ezdeck::kNumLiveTracks;
    std::array<std::unique_ptr<amanorsac::perform::PerformProcessor>, kNumStrips> strips;
    std::array<std::atomic<bool>, kNumStrips> stripOn {};
    std::array<std::unique_ptr<juce::DocumentWindow>, kNumStrips> stripWindows;
    juce::MidiBuffer stripMidi;   // always empty; the strip takes no MIDI

    // ---- live tracks: instruments (PX-D) and inputs (PX-C) ----
    ezinst::PluginLibrary                                     pluginLibrary;
    std::array<ezinst::InstrumentSlot, ezdeck::kNumLiveTracks> instruments;
    ezinst::HostPlayHead                                      hostPlayHead;
    std::array<std::unique_ptr<ezinst::InstrumentEditorWindow>, ezdeck::kNumLiveTracks> instrumentWindows;
    std::unique_ptr<juce::DocumentWindow>                     pluginListWindow;
    std::unique_ptr<juce::ThreadWithProgressWindow>           pluginScan;

    // Per live track: -1 no input, else the device input channel (see
    // SettingsSnapshot); MIDI channel 0 = every channel. Written on the
    // message thread, read per block / per MIDI message.
    std::array<std::atomic<int>,  ezdeck::kNumLiveTracks> trackInput {};
    std::array<std::atomic<bool>, ezdeck::kNumLiveTracks> trackStereo {};
    std::array<std::atomic<int>,  ezdeck::kNumLiveTracks> trackMidiChannel {};
    std::array<std::vector<float>, ezdeck::kNumLiveTracks> trackScratchL, trackScratchR;   // sized in prepareToPlay
    std::vector<std::vector<float>> liveInScratch;     // one per open input channel, sized in prepareToPlay
    std::atomic<int>                liveInputCount { 0 };
    bool                            reconfiguringInputs { false };

    // ---- Guide.h state ----
    struct CueSource { std::vector<float> mono; double rate { 48000.0 }; };
    std::vector<CueSource>            cueSources;      // as loaded
    std::vector<std::vector<float>>   cueDeviceAudio;  // at the device rate
    std::vector<ezguide::CueSample>   cueBank;         // what the player reads
    std::map<std::string, int>        cueIds;
    juce::StringArray                 cueMenuNames;    // "Song Form/Chorus-2"
    ezguide::Click                    songClick;       // the native click (guideClick() is the per-song switch)
    ezguide::CuePlayer                cuePlayer;       // audio thread
    std::vector<float>                cueScratchL, cueScratchR;
    std::unique_ptr<std::vector<ezguide::CueEvent>>              currentSchedule;
    std::vector<std::unique_ptr<std::vector<ezguide::CueEvent>>> retiredSchedules;
    std::atomic<const std::vector<ezguide::CueEvent>*>           activeSchedule { nullptr };
    std::atomic<const std::vector<ezguide::CueEvent>*>           audioSeenSchedule { nullptr };
    const std::vector<ezguide::CueEvent>*                        guideScheduleInUse { nullptr };   // audio thread
    std::atomic<bool>   metronomeGate { false };   // Settings > Metronome: the PERFORM loop-mode click
    std::atomic<int>    guideFlags { 0 };          // bit 0 click, bit 1 cues
    std::atomic<int>    guideDeck { -1 };
    std::atomic<int>    guideBeatsPerBar { 4 };
    std::atomic<double> guideSamplesPerBar { 0.0 };
    std::atomic<bool>   guideCountFast { true };
    std::atomic<int>    pendingCuePreview { ezguide::kNoCue };
    int64_t             lastCountInBeat { INT64_MIN };   // audio thread
    std::atomic<float>  guideDebugPeak { 0.0f };          // loudest click sample this block (PERFORMLIVE_GUIDE_DEBUG)
    int                 guideDebugTicks { 0 };

    // Lazily-created browser (see the constructor): this container reports
    // its first show so the WebView2 process is only started when wanted.
    struct LazyHolder : public juce::Component
    {
        std::function<void()> onFirstShow;
        void visibilityChanged() override
        {
            if (isVisible() && ! shown) { shown = true; if (onFirstShow) onFirstShow(); }
        }
        bool shown { false };
    };
    LazyHolder browserHolder;
    std::unique_ptr<ezweb::BrowserTab> browserTab;

    void createBrowserTab()
    {
        if (browserTab) return;
        juce::Logger::writeToLog ("browser: creating tab");
        browserTab = std::make_unique<ezweb::BrowserTab>();
        juce::Logger::writeToLog ("browser: tab created");
        browserTab->onImportFiles = [this] (juce::Array<juce::File> files)
        {
            if (libraryTab != nullptr) libraryTab->importFiles (files);
        };
        browserTab->onMessage = [this] (juce::String text) { showToast (text); };
        browserTab->getRightsAccepted = [] { auto* p = getAppSettings(); return p != nullptr && p->getBoolValue ("urlImportRightsAccepted", false); };
        browserTab->setRightsAccepted = [] { if (auto* p = getAppSettings()) { p->setValue ("urlImportRightsAccepted", true); p->saveIfNeeded(); } };
        browserHolder.addAndMakeVisible (*browserTab);
        browserTab->setBounds (browserHolder.getLocalBounds());
        pushWebLevel();   // the mixer's WEB strip applies to this page too
    }

    // ---- song <-> deck helpers ---------------------------------------------

    int currentSongDeck() const
    {
        return (setlistPos >= 0 && setlistPos < (int) setlist.size()) ? setlist[(size_t) setlistPos] : -1;
    }

    /** The tempo this song plays at: "play at" override > the user-set
        original > master. Never a detected value. */
    double deckBpm (int deck) const
    {
        if (deck < 0 || deck >= kNumDecks) return masterTempo.bpm;
        if (auto t = deckTempoOverride[(size_t) deck]; t.has_value() && *t > 0.0) return *t;
        if (auto s = deckSourceBpm[(size_t) deck]; s.has_value() && *s > 0.0) return *s;
        return masterTempo.bpm;
    }

    /** One bar, in the deck's own playhead units. 0 if unknown. */
    double deckSamplesPerBar (int deck) const
    {
        if (deck < 0 || deck >= kNumDecks) return 0.0;
        return ezarr::samplesPerBarInDeckUnits (deckBpm (deck), session.deckTempo (deck).beatsPerBar,
                                                currentSampleRate, session.decks[(size_t) deck].getRateRatio());
    }

    int deckLengthBars (int deck) const
    {
        if (deck < 0 || deck >= kNumDecks) return 0;
        int bars = 0;
        for (int l = 0; l < ezdeck::kNumLayers; ++l)
        {
            const auto& layer = session.decks[(size_t) deck].layers[(size_t) l];
            if (layer.loaded) bars = (std::max) (bars, layer.stemBarLength);
        }
        if (bars == 0)
        {
            const double spb = deckSamplesPerBar (deck);
            if (spb > 0.0) bars = (int) std::ceil ((double) session.decks[(size_t) deck].stemLength() / spb);
        }
        return bars;
    }

    void ensureArrangementLength (int deck)
    {
        if (deck < 0 || deck >= kNumDecks) return;
        auto& a = arrangements[(size_t) deck];
        const int bars = deckLengthBars (deck);
        if (bars > 0) a.lengthBars = bars;
    }

    /** Makes the session's clock run at the song's tempo, so "next bar",
        the metronome and the count-in all mean the song's bar. */
    void adoptSongTempo (int deck)
    {
        const double bpm = deckBpm (deck);
        if (bpm > 0.0 && std::abs (bpm - masterTempo.bpm) > 0.01)
        {
            masterTempo.bpm = bpm;
            session.setTempo (masterTempo);
        }
    }

    // ---- PlaybackHost: setlist -----------------------------------------------

    int numSongs() const override { return (int) setlist.size(); }
    int songDeck (int i) const override { return (i >= 0 && i < (int) setlist.size()) ? setlist[(size_t) i] : -1; }
    juce::String songName (int i) const override { const int d = songDeck (i); return d >= 0 ? deckLabel (d) : juce::String(); }
    juce::Colour songColour (int i) const override
    {
        const int d = songDeck (i);
        if (d >= 0 && rowColourSet[(size_t) d]) return juce::Colour (rowColourArgb[(size_t) d]);
        return juce::Colour (ezarr::defaultSectionColour (i));
    }
    bool songHasAudio (int i) const override
    {
        const int d = songDeck (i);
        if (d < 0) return false;
        for (int l = 0; l < ezdeck::kNumLayers; ++l) if (session.decks[(size_t) d].layers[(size_t) l].loaded) return true;
        return false;
    }
    int songLengthBars (int i) const override
    {
        const int d = songDeck (i);
        if (d < 0) return 0;
        return arrangements[(size_t) d].lengthBars > 0 ? arrangements[(size_t) d].lengthBars : deckLengthBars (d);
    }
    double songLengthSeconds (int i) const override
    {
        const int d = songDeck (i);
        const double bpm = deckBpm (d);
        return bpm > 0.0 ? songLengthBars (i) * (60.0 / bpm) * session.deckTempo (d).beatsPerBar : 0.0;
    }
    int currentSongIndex() const override { return setlistPos; }

    void selectSong (int i) override
    {
        if (i < 0 || i >= (int) setlist.size()) return;
        const int deck = setlist[(size_t) i];
        setlistPos = i;
        guideRowOverride = -1;   // the setlist song is the guide's song again
        lastSectionIdx = -1;
        queuedSectionIdx = -1;
        session.cancelSeek();
        session.cancelStop();
        session.decks[(size_t) deck].clearSectionLoop();
        adoptSongTempo (deck);
        ensureArrangementLength (deck);
        // A cued song replaces whatever was active. Stopped: switch now so
        // Play starts it. Playing: switch now too -- "next song" on stage
        // means now, and the engine resets the new deck's playhead.
        session.switchNow (deck);
        rebuildGuideSchedule();
        if (playbackView) playbackView->songChanged();
    }

    void moveSong (int from, int to) override
    {
        if (from < 0 || from >= (int) setlist.size() || to < 0 || to >= (int) setlist.size() || from == to) return;
        const int v = setlist[(size_t) from];
        setlist.erase (setlist.begin() + from);
        setlist.insert (setlist.begin() + to, v);
        if (setlistPos == from) setlistPos = to;
        else if (from < setlistPos && to >= setlistPos) --setlistPos;
        else if (from > setlistPos && to <= setlistPos) ++setlistPos;
        if (playbackView) playbackView->songChanged();
    }

    void removeSong (int i) override
    {
        if (i < 0 || i >= (int) setlist.size()) return;
        setlist.erase (setlist.begin() + i);
        if (setlist.empty()) setlistPos = -1;
        else if (setlistPos >= (int) setlist.size()) setlistPos = (int) setlist.size() - 1;
        else if (i < setlistPos) --setlistPos;
        if (playbackView) playbackView->songChanged();
    }

    void promptAddSong() override
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Add a deck row as a song");
        int count = 0;
        for (int d = 0; d < kNumDecks; ++d)
        {
            bool loaded = false;
            for (int l = 0; l < ezdeck::kNumLayers; ++l) if (session.decks[(size_t) d].layers[(size_t) l].loaded) { loaded = true; break; }
            if (! loaded) continue;
            const bool already = std::find (setlist.begin(), setlist.end(), d) != setlist.end();
            m.addItem (d + 1, deckLabel (d) + "  (" + juce::String (deckLengthBars (d)) + " bars)", ! already, already);
            ++count;
        }
        if (count == 0)
        {
            showToast ("Load audio into a deck row on PERFORM first, then add it here as a song.");
            return;
        }
        m.showMenuAsync (juce::PopupMenu::Options(), [this] (int r)
        {
            if (r <= 0) return;
            const int d = r - 1;
            if (d < 0 || d >= kNumDecks) return;
            auto& deck = session.decks[(size_t) d];
            if (deck.mode != ezdeck::DeckMode::stem)
            {
                if (! canMutateDeckState (d)) { showToast (deckLabel (d) + ": stop it first -- a song plays as a stem, not a loop"); return; }
                deck.mode = ezdeck::DeckMode::stem;
                refreshSlotLabels();
            }
            // The setlist decides what comes next, not the engine's own
            // auto-advance -- that would skip to "the next deck with content",
            // which is not the same as the next song.
            deck.stemEndBehavior = ezdeck::StemEndBehavior::next;
            setlist.push_back (d);
            ensureArrangementLength (d);
            if (setlistPos < 0) selectSong ((int) setlist.size() - 1);
            if (playbackView) playbackView->songChanged();
        });
    }

    void renameSong (int i) override
    {
        const int d = songDeck (i);
        if (d < 0) return;
        auto* aw = new juce::AlertWindow ("Rename song", "", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", rowName[(size_t) d], "Name");
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, d, aw] (int r)
        {
            std::unique_ptr<juce::AlertWindow> owner (aw);
            if (r != 1) return;
            rowName[(size_t) d] = aw->getTextEditorContents ("name").trim();
            refreshSlotLabels();
            if (playbackView) playbackView->songChanged();
        }), false);
    }

    // ---- PlaybackHost: arrangement ---------------------------------------------

    ezarr::Arrangement* currentArrangement() override
    {
        const int d = currentSongDeck();
        if (d < 0) return nullptr;
        ensureArrangementLength (d);
        return &arrangements[(size_t) d];
    }

    void arrangementEdited() override
    {
        lastSectionIdx = -1;   // re-evaluate loopOnEntry/pauseAfter for the section we are in
        rebuildGuideSchedule();
    }

    // ---- PlaybackHost: native click and cues (Guide.h) ---------------------------

    bool guideClick() const override { const int d = currentSongDeck(); return d >= 0 && arrangements[(size_t) d].guideClick; }
    bool guideCues() const override  { const int d = currentSongDeck(); return d >= 0 && arrangements[(size_t) d].guideCues; }
    int  cueLeadBars() const override { const int d = currentSongDeck(); return d >= 0 ? arrangements[(size_t) d].cueLeadBars : 2; }
    bool cueCounts() const override  { const int d = currentSongDeck(); return d < 0 || arrangements[(size_t) d].cueCounts; }
    void setGuideClick (bool on) override  { const int d = currentSongDeck(); if (d >= 0) { arrangements[(size_t) d].guideClick = on; rebuildGuideSchedule(); } }
    void setGuideCues (bool on) override   { const int d = currentSongDeck(); if (d >= 0) { arrangements[(size_t) d].guideCues = on; rebuildGuideSchedule(); } }
    void setCueLeadBars (int b) override   { const int d = currentSongDeck(); if (d >= 0) { arrangements[(size_t) d].cueLeadBars = juce::jlimit (1, 8, b); rebuildGuideSchedule(); } }
    void setCueCounts (bool on) override   { const int d = currentSongDeck(); if (d >= 0) { arrangements[(size_t) d].cueCounts = on; rebuildGuideSchedule(); } }
    bool cueBankLoaded() const override    { return ! cueBank.empty(); }
    juce::StringArray cueNames() const override { return cueMenuNames; }

    void previewCue (const juce::String& stem) override
    {
        const int id = cueIdFor (stem.toStdString());
        if (id != ezguide::kNoCue) pendingCuePreview.store (id, std::memory_order_relaxed);
    }

    /** PLAYBACK's Auto-section: listen (speech, else the cue recordings), then review. */
    void listenForSections (int i, int layer) override
    {
        const int d = songDeck (i);
        if (d >= 0) listenForSectionsOnDeck (d, layer);
    }

    bool songHasGuideTrack (int i) const override
    {
        const int d = songDeck (i);
        return d >= 0 && songGuideTrack[(size_t) d] != nullptr;
    }

    int cueIdFor (const std::string& stem) const
    {
        auto it = cueIds.find (stem);
        return it == cueIds.end() ? ezguide::kNoCue : it->second;
    }

    int countCueIdFor (int beat, bool fast) const
    {
        static const char* const words[] = { "1", "2", "3", "4", "5", "6", "7", "8" };
        if (beat < 1 || beat > 8) return ezguide::kNoCue;
        return cueIdFor (std::string (words[beat - 1]) + (fast ? "-fast" : "-slow"));
    }

    /** Loads every WAV under the cue folder once (mono, at its own rate).
        Looked for beside the exe first, then in the working directory. */
    void loadCueBank()
    {
        const juce::File exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        const juce::File candidates[] = {
            exeDir.getChildFile ("cues"),
            exeDir.getChildFile ("MotionWorshipGuideCues"),
            juce::File::getCurrentWorkingDirectory().getChildFile ("cues"),
            juce::File::getCurrentWorkingDirectory().getChildFile ("MotionWorshipGuideCues"),
        };
        juce::File folder;
        for (const auto& c : candidates) if (c.isDirectory()) { folder = c; break; }
        if (folder == juce::File()) { juce::Logger::writeToLog ("Cues: no cue folder found -- native cues unavailable"); return; }

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        for (const auto& group : { "Song Form", "Counts", "Instrumentation" })
        {
            auto dir = folder.getChildFile (group);
            auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav");
            files.sort();
            for (const auto& f : files)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
                if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > 48000 * 10) continue;
                juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
                CueSource src;
                src.rate = reader->sampleRate;
                src.mono.resize ((size_t) buf.getNumSamples());
                for (int i = 0; i < buf.getNumSamples(); ++i)
                {
                    float s = 0.0f;
                    for (int ch = 0; ch < buf.getNumChannels(); ++ch) s += buf.getSample (ch, i);
                    src.mono[(size_t) i] = s / (float) juce::jmax (1, buf.getNumChannels());
                }
                const auto stem = f.getFileNameWithoutExtension().toStdString();
                cueIds[stem] = (int) cueSources.size();
                cueSources.push_back (std::move (src));
                if (juce::String (group) != "Counts") cueMenuNames.add (juce::String (group) + "/" + juce::String (stem));
            }
        }
        juce::Logger::writeToLog ("Cues: " + juce::String ((int) cueSources.size()) + " recordings from " + folder.getFullPathName());
    }

    /** Resamples the bank to the device rate (linear -- speech, and the
        rates are close) and rebuilds the pointer table the player reads. */
    void prepareCueBank (double deviceRate)
    {
        cueDeviceAudio.assign (cueSources.size(), {});
        cueBank.assign (cueSources.size(), {});
        for (size_t i = 0; i < cueSources.size(); ++i)
        {
            const auto& src = cueSources[i];
            const double ratio = src.rate > 0.0 ? src.rate / deviceRate : 1.0;
            const int outLen = (int) std::floor ((double) src.mono.size() / ratio);
            auto& out = cueDeviceAudio[i];
            out.resize ((size_t) juce::jmax (0, outLen));
            for (int o = 0; o < outLen; ++o)
            {
                const double p = (double) o * ratio;
                const size_t a = (size_t) p;
                const float t = (float) (p - (double) a);
                const float s0 = src.mono[juce::jmin (a, src.mono.size() - 1)];
                const float s1 = src.mono[juce::jmin (a + 1, src.mono.size() - 1)];
                out[(size_t) o] = s0 + (s1 - s0) * t;
            }
            cueBank[i] = { out.data(), (int) out.size() };
        }
        cuePlayer.setBank (&cueBank);
    }

    /** Message thread. Publishes a fresh schedule for the current song; the
        audio thread picks it up at its next block. Old schedules are kept
        until the audio thread has confirmed it moved on, then freed. */
    void rebuildGuideSchedule()
    {
        // PERFORM: the row last triggered from the grid gets the guide even
        // when it isn't the setlist song (guideRowOverride); PLAYBACK's
        // selectSong() clears that.
        const int d = (guideRowOverride >= 0 && guideRowOverride < kNumDecks) ? guideRowOverride : currentSongDeck();
        auto fresh = std::make_unique<std::vector<ezguide::CueEvent>>();
        int flags = 0;
        int songBits = 0;
        double spb = 0.0;
        int beats = 4;
        if (d >= 0)
        {
            const auto& a = arrangements[(size_t) d];
            spb   = deckSamplesPerBar (d);
            beats = juce::jmax (1, session.deckTempo (d).beatsPerBar);
            const bool songClickHere = useSongClick[(size_t) d] && songClickTrack[(size_t) d] != nullptr;
            const bool songGuideHere = useSongGuide[(size_t) d] && songGuideTrack[(size_t) d] != nullptr;
            if (songClickHere) songBits |= 1;
            if (songGuideHere) songBits |= 2;
            guideDeckFileRate.store (deckFileSampleRate[(size_t) d], std::memory_order_relaxed);
            if (a.guideClick) flags |= 1;
            if (a.guideCues && (songGuideHere || ! cueBank.empty()))
            {
                flags |= 2;
                std::vector<ezguide::GuideSection> secs;
                for (const auto& s : a.sections) secs.push_back ({ s.name, s.cue, s.startBar });
                ezguide::ScheduleSettings st;
                st.samplesPerBar = spb;
                st.beatsPerBar   = beats;
                st.leadBars      = a.cueLeadBars;
                st.counts        = a.cueCounts;
                st.fast          = deckBpm (d) >= 100.0;
                *fresh = ezguide::buildSchedule (secs,
                                                 st,
                                                 [this] (const std::string& stem) { return cueIdFor (stem); },
                                                 [this] (int beat, bool fast) { return countCueIdFor (beat, fast); });
            }
        }
        guideSamplesPerBar.store (spb, std::memory_order_relaxed);
        guideBeatsPerBar.store (beats, std::memory_order_relaxed);
        guideSongTracks.store (songBits, std::memory_order_relaxed);
        guideFlags.store (flags, std::memory_order_relaxed);
        guideDeck.store (d, std::memory_order_relaxed);
        guideCountFast.store (d >= 0 && deckBpm (d) >= 100.0, std::memory_order_relaxed);

        retiredSchedules.push_back (std::move (currentSchedule));
        currentSchedule = std::move (fresh);
        activeSchedule.store (currentSchedule.get(), std::memory_order_release);
        // free retired schedules once the audio thread is provably past them
        if (audioSeenSchedule.load (std::memory_order_acquire) == currentSchedule.get()) retiredSchedules.clear();
        if (retiredSchedules.size() > 64) retiredSchedules.erase (retiredSchedules.begin());   // never grows without bound
    }

    /** Audio thread. Fills metroScratch (click) and cueScratch (voice). */
    void renderGuide (int numSamples, int64_t masterPosAtStart, double deckPosBefore)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            metroScratchL[(size_t) i] = 0.0f; metroScratchR[(size_t) i] = 0.0f;
            cueScratchL[(size_t) i] = 0.0f;   cueScratchR[(size_t) i] = 0.0f;
        }

        // a manual "Say now" from the menu plays whatever is happening
        if (const int pv = pendingCuePreview.exchange (ezguide::kNoCue, std::memory_order_relaxed); pv != ezguide::kNoCue)
            cuePlayer.trigger (pv);

        const bool running = transportRunning.load();
        const int  flags   = guideFlags.load (std::memory_order_relaxed);
        const int  d       = guideDeck.load (std::memory_order_relaxed);
        const bool songLive = running && d >= 0 && d < kNumDecks && session.activeDeck() == d;

        if (const auto* s = activeSchedule.load (std::memory_order_acquire); s != guideScheduleInUse)
        {
            guideScheduleInUse = s;
            cuePlayer.setSchedule (s);
            cuePlayer.locate (deckPosBefore);
            audioSeenSchedule.store (s, std::memory_order_release);
        }

        if (songLive)
        {
            const double spb   = guideSamplesPerBar.load (std::memory_order_relaxed);
            const int    beats = juce::jmax (1, guideBeatsPerBar.load (std::memory_order_relaxed));
            const int64_t countInLeft = session.countInSamplesLeft();

            if (countInLeft > 0)
            {
                // Count-in: the master clock, counted backwards to the downbeat,
                // so the last count-in bar ends exactly where the song starts.
                const double beatLen = ezdeck::barLengthSamples (session.getTempo(), currentSampleRate) / (double) beats;
                const double pos = -(double) countInLeft;
                // a count-in is always clicked: that is what a count-in is for
                songClick.render (metroScratchL.data(), metroScratchR.data(), numSamples, pos, beatLen, beats);
                if (flags & 2)
                {
                    // speak "1, 2, 3, 4" on the beats of the final count-in bar
                    for (int i = 0; i < numSamples; ++i)
                    {
                        const int64_t beat = (int64_t) std::floor ((pos + (double) i) / beatLen);
                        if (beat != lastCountInBeat)
                        {
                            lastCountInBeat = beat;
                            if (beat >= -beats && beat < 0)
                                cuePlayer.trigger (countCueIdFor ((int) (beat + beats) + 1, guideCountFast.load (std::memory_order_relaxed)));
                        }
                    }
                }
                cuePlayer.locate (0.0);
            }
            else
            {
                lastCountInBeat = INT64_MIN;
                const double posAfter = session.decks[(size_t) d].playheadPosition();
                // the song's own click/guide tracks when it has them and they're chosen; else the built-in ones
                const int songBits = guideSongTracks.load (std::memory_order_relaxed);
                const double fileRate = guideDeckFileRate.load (std::memory_order_relaxed);
                if (flags & 1)
                {
                    const SongTrack* ct = (songBits & 1) ? liveClickTrack[(size_t) d].load (std::memory_order_acquire) : nullptr;
                    if (ct != nullptr) renderSongTrack (*ct, metroScratchL.data(), metroScratchR.data(), numSamples, deckPosBefore, fileRate);
                    else if (spb > 0.0) songClick.render (metroScratchL.data(), metroScratchR.data(), numSamples, deckPosBefore, spb / (double) beats, beats);
                }
                if (flags & 2)
                {
                    const SongTrack* gt = (songBits & 2) ? liveGuideTrack[(size_t) d].load (std::memory_order_acquire) : nullptr;
                    if (gt != nullptr) renderSongTrack (*gt, cueScratchL.data(), cueScratchR.data(), numSamples, deckPosBefore, fileRate);
                    else cuePlayer.advance (deckPosBefore, posAfter);
                }
            }
        }
        else
        {
            lastCountInBeat = INT64_MIN;
            // PERFORM's loop mode keeps the plain metronome on the Click strip.
            if (running && metronomeGate.load (std::memory_order_relaxed))
                metro.render (metroScratchL.data(), metroScratchR.data(), numSamples, masterPosAtStart, session.getTempo().bpm);
            else if (! running)
                songClick.reset();
        }

        cuePlayer.render (cueScratchL.data(), cueScratchR.data(), numSamples);
        float pk = 0.0f;
        for (int i = 0; i < numSamples; ++i) pk = juce::jmax (pk, std::fabs (metroScratchL[(size_t) i]));
        guideDebugPeak.store (juce::jmax (pk, guideDebugPeak.load (std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);
    }

    // ---- PlaybackHost: state ---------------------------------------------------

    bool   isPlaying() const override    { return transportRunning.load(); }
    bool   isCountingIn() const override { return session.isCountingIn(); }

    double currentBar() const override
    {
        const int d = currentSongDeck();
        const double spb = deckSamplesPerBar (d);
        if (d < 0 || spb <= 0.0) return 0.0;
        return ezarr::deckSamplesToBar (session.decks[(size_t) d].playheadPosition(), spb);
    }

    int currentSection() const override
    {
        const int d = currentSongDeck();
        if (d < 0) return -1;
        return arrangements[(size_t) d].sectionAtBar (currentBar());
    }

    int    queuedSection() const override   { return session.hasPendingSeek() ? queuedSectionIdx : -1; }
    bool   isLoopingSection() const override { const int d = currentSongDeck(); return d >= 0 && session.decks[(size_t) d].isSectionLoopEnabled(); }
    double tempoBpm() const override         { return deckBpm (currentSongDeck()); }
    int    beatsPerBar() const override      { const int d = currentSongDeck(); return d >= 0 ? session.deckTempo (d).beatsPerBar : masterTempo.beatsPerBar; }
    double secondsPerBar() const override    { const double bpm = tempoBpm(); return bpm > 0.0 ? (60.0 / bpm) * beatsPerBar() : 0.0; }
    int    jumpMode() const override         { return jumpModeValue; }
    void   setJumpMode (int m) override      { jumpModeValue = juce::jlimit (0, 2, m); }
    int    countInBars() const override      { const int d = currentSongDeck(); return d >= 0 ? arrangements[(size_t) d].countInBars : 0; }
    void   setCountInBars (int b) override   { const int d = currentSongDeck(); if (d >= 0) arrangements[(size_t) d].countInBars = juce::jlimit (0, 4, b); }

    const std::vector<float>* layerSamples (int i, int layer) const override
    {
        const int d = songDeck (i);
        if (d < 0 || layer < 0 || layer >= ezdeck::kNumLayers) return nullptr;
        const auto& l = session.decks[(size_t) d].layers[(size_t) layer];
        return l.loaded ? &l.left : nullptr;
    }

    bool layerIsLive (int i, int layer) const override
    {
        juce::ignoreUnused (i, layer);
        return false;   // decks are stems only now; live sources are the mixer's LIVE tracks
    }

    juce::String layerName (int i, int layer) const override
    {
        const int d = songDeck (i);
        if (d < 0 || layer < 0 || layer >= ezdeck::kNumLayers) return {};
        if (layerNameOverride[(size_t) d][(size_t) layer].isNotEmpty()) return layerNameOverride[(size_t) d][(size_t) layer];
        const auto& path = layerFilePaths[(size_t) d][(size_t) layer];
        if (path.isEmpty()) return "Stem " + juce::String (layer + 1);
        // Stem packs name files "SONG 105BPM - Clap": every track in a song
        // shares the part before the dash, so the lane header shows the part
        // after it (and the icon is chosen from that). The full name stays
        // visible inside the lane via layerFileName().
        auto name = displayFileName (juce::File (path).getFileNameWithoutExtension());
        const int dash = name.lastIndexOf (" - ");
        if (dash > 0 && name.substring (dash + 3).trim().isNotEmpty())
            name = name.substring (dash + 3).trim();
        return name;
    }

    bool layerEnabled (int i, int layer) const override
    {
        const int d = songDeck (i);
        return d >= 0 && layer >= 0 && layer < ezdeck::kNumLayers
            && session.decks[(size_t) d].layers[(size_t) layer].enabled.load (std::memory_order_relaxed);
    }

    void toggleLayer (int i, int layer) override
    {
        const int d = songDeck (i);
        if (d < 0 || layer < 0 || layer >= ezdeck::kNumLayers) return;
        auto& l = session.decks[(size_t) d].layers[(size_t) layer];
        l.enabled.store (! l.enabled.load (std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // Track names set from the PLAYBACK lane headers are the same per-slot
    // names PERFORM's slot editor sets (layerNameOverride), so they show in
    // both places and save with the project like every other slot name.
    void setLayerName (int i, int layer, const juce::String& name) override
    {
        const int d = songDeck (i);
        if (d < 0 || layer < 0 || layer >= ezdeck::kNumLayers) return;
        layerNameOverride[(size_t) d][(size_t) layer] = name.trim().substring (0, 40);
        refreshSlotLabels();
        if (playbackView) playbackView->songChanged();
    }

    bool layerHasCustomName (int i, int layer) const override
    {
        const int d = songDeck (i);
        return d >= 0 && layer >= 0 && layer < ezdeck::kNumLayers
            && layerNameOverride[(size_t) d][(size_t) layer].isNotEmpty();
    }

    juce::String layerFileName (int i, int layer) const override
    {
        const int d = songDeck (i);
        if (d < 0 || layer < 0 || layer >= ezdeck::kNumLayers) return {};
        const auto& path = layerFilePaths[(size_t) d][(size_t) layer];
        return path.isNotEmpty() ? displayFileName (juce::File (path).getFileNameWithoutExtension()) : juce::String();
    }

    // ---- PlaybackHost: commands ------------------------------------------------

    void playStop() override
    {
        const int d = currentSongDeck();
        if (d >= 0 && ! transportRunning.load())
        {
            // Start THIS song, from the top, at its tempo -- regardless of what
            // PERFORM last had active.
            adoptSongTempo (d);
            session.resetTransport();
            session.switchNow (d);
            lastSectionIdx = -1;
            rebuildGuideSchedule();   // the click and cues follow THIS song from its first sample
            transportRunning = true;
            return;
        }
        actionRegistry.invoke (ezaction::ActionId::PlayStop);
        if (! transportRunning.load()) endCountInIfAny();
    }

    void playWithCountIn() override
    {
        const int d = currentSongDeck();
        if (d < 0) { playStop(); return; }
        if (transportRunning.load()) { playStop(); return; }

        const int bars = arrangements[(size_t) d].countInBars > 0 ? arrangements[(size_t) d].countInBars : 1;
        adoptSongTempo (d);
        session.resetTransport();
        session.switchNow (d);
        lastSectionIdx = -1;
        rebuildGuideSchedule();

        // The metronome IS the count-in. Unmute it for the duration even if
        // the performer normally runs without a click; restored afterwards.
        // the count-in is always clicked (renderGuide), so nothing to unmute now
        countInActive = true;

        session.startCountIn ((int64_t) bars * ezdeck::barLengthSamples (session.getTempo(), currentSampleRate));
        transportRunning = true;
    }

    void endCountInIfAny()
    {
        if (! countInActive) return;
        countInActive = false;
        // (Click strip mute untouched -- see startCountIn)
    }

    void jumpToSection (int s) override
    {
        const int d = currentSongDeck();
        if (d < 0) return;
        auto& a = arrangements[(size_t) d];
        if (s < 0 || s >= (int) a.sections.size()) return;
        const double spb = deckSamplesPerBar (d);
        if (spb <= 0.0) return;

        const double target = ezarr::barToDeckSamples (a.sectionStartBar (s), spb);

        if (! transportRunning.load())
        {
            // Stopped: just park there. Play resets the transport, so a
            // stopped jump is a starting point, not a queued event.
            session.decks[(size_t) d].seekTo (target);
            queuedSectionIdx = -1;
            return;
        }

        queuedSectionIdx = s;
        switch (jumpModeValue)
        {
            case 2:  session.queueSeek (target, ezdeck::Session<kNumDecks>::SeekWhen::now); break;
            case 0:  session.queueSeek (target, ezdeck::Session<kNumDecks>::SeekWhen::nextBar); break;
            default:
            {
                const int cur = currentSection();
                const double fireAt = cur >= 0 ? ezarr::barToDeckSamples (a.sectionEndBar (cur), spb)
                                               : ezarr::barToDeckSamples (std::ceil (currentBar()), spb);
                session.queueSeek (target, ezdeck::Session<kNumDecks>::SeekWhen::atDeckPosition, fireAt);
                break;
            }
        }
        // Leaving a looped section by choice cancels the loop -- otherwise
        // the jump would fire and the loop would drag the playhead back.
        session.decks[(size_t) d].clearSectionLoop();
    }

    void jumpNow() override
    {
        const int d = currentSongDeck();
        if (d < 0 || queuedSectionIdx < 0 || ! session.hasPendingSeek()) return;
        session.queueSeek (session.pendingSeekTarget(), ezdeck::Session<kNumDecks>::SeekWhen::nextBar);
    }

    void cancelJump() override
    {
        session.cancelSeek();
        queuedSectionIdx = -1;
    }

    void toggleLoopSection() override
    {
        const int d = currentSongDeck();
        if (d < 0) return;
        auto& deck = session.decks[(size_t) d];
        if (deck.isSectionLoopEnabled()) { deck.clearSectionLoop(); showToast ("Loop off"); return; }

        const int cur = currentSection();
        const auto& a = arrangements[(size_t) d];
        const double spb = deckSamplesPerBar (d);
        if (cur < 0 || spb <= 0.0) { showToast ("No section here to loop -- add sections on the timeline"); return; }
        deck.setSectionLoop (ezarr::barToDeckSamples (a.sectionStartBar (cur), spb),
                             ezarr::barToDeckSamples (a.sectionEndBar (cur), spb));
        showToast ("Looping " + juce::String (juce::CharPointer_UTF8 (a.sections[(size_t) cur].name.c_str())));
    }

    void nextSection() override
    {
        const int d = currentSongDeck();
        if (d < 0) return;
        const auto& a = arrangements[(size_t) d];
        const int from = queuedSection() >= 0 ? queuedSection() : currentSection();
        const int n = a.nextPlayableAfter (from);
        if (n >= 0) jumpToSection (n);
        else if (setlistPos + 1 < (int) setlist.size()) nextSong();
    }

    void prevSection() override
    {
        const int d = currentSongDeck();
        if (d < 0) return;
        const auto& a = arrangements[(size_t) d];
        const int cur = currentSection();
        // Early in a section "previous" means the previous one; late in it,
        // it means the top of this one -- the way every rewind button works.
        const double into = currentBar() - a.sectionStartBar (cur);
        const int p = (cur >= 0 && into < 1.0) ? a.prevPlayableBefore (cur) : cur;
        if (p >= 0) jumpToSection (p);
    }

    void nextSong() override
    {
        if (setlistPos + 1 >= (int) setlist.size()) return;
        const bool wasPlaying = transportRunning.load();
        endCountInIfAny();
        selectSong (setlistPos + 1);
        if (wasPlaying) { session.resetTransport(); session.switchNow (currentSongDeck()); }
    }

    void prevSong() override
    {
        if (setlistPos <= 0) return;
        const bool wasPlaying = transportRunning.load();
        endCountInIfAny();
        selectSong (setlistPos - 1);
        if (wasPlaying) { session.resetTransport(); session.switchNow (currentSongDeck()); }
    }

    void seekToBar (double bar) override
    {
        const int d = currentSongDeck();
        const double spb = deckSamplesPerBar (d);
        if (d < 0 || spb <= 0.0) return;
        const double target = ezarr::barToDeckSamples ((std::max) (0.0, bar), spb);
        if (transportRunning.load()) { queuedSectionIdx = -1; session.queueSeek (target, ezdeck::Session<kNumDecks>::SeekWhen::nextBar); }
        else session.decks[(size_t) d].seekTo (target);
    }

    // ---- the per-tick scheduler ------------------------------------------------
    //
    // Runs on the UI timer (15 Hz). Nothing here is sample-critical: the
    // exact moments -- the jump, the loop wrap, the stop -- are all resolved
    // on the audio thread by Session. This only ARMS the next one when the
    // playhead enters a section, and reacts to things the engine reports.

    void serviceSectionPlayback()
    {
        // An armed stop fired: the engine is already silent and frozen at
        // the exact sample; all that is left is to agree with it.
        if (session.consumeStopRequest())
        {
            transportRunning = false;
            endCountInIfAny();
            session.clearHalt();
            showToast ("Paused at end of section");
        }

        if (! transportRunning.load()) return;

        if (countInActive && ! session.isCountingIn()) endCountInIfAny();

        const int d = currentSongDeck();
        if (d < 0 || session.activeDeck() != d) return;

        if (queuedSectionIdx >= 0 && ! session.hasPendingSeek()) queuedSectionIdx = -1;

        auto& a = arrangements[(size_t) d];
        const int cur = currentSection();
        if (cur != lastSectionIdx)
        {
            lastSectionIdx = cur;
            if (cur >= 0 && cur < (int) a.sections.size())
            {
                const auto& sec = a.sections[(size_t) cur];
                const double spb = deckSamplesPerBar (d);
                if (spb > 0.0)
                {
                    if (sec.loopOnEntry && ! session.decks[(size_t) d].isSectionLoopEnabled())
                        session.decks[(size_t) d].setSectionLoop (ezarr::barToDeckSamples (a.sectionStartBar (cur), spb),
                                                                   ezarr::barToDeckSamples (a.sectionEndBar (cur), spb));
                    if (sec.pauseAfter)
                        session.armStopAt (ezarr::barToDeckSamples (a.sectionEndBar (cur), spb));
                    else
                        session.cancelStop();
                }
            }
        }

        // End of the song: the setlist decides, not the engine's own
        // "next deck with content" rule (stemEndBehavior is set to 'next'
        // for every setlist song so the engine just finishes cleanly).
        if (! session.isCountingIn() && session.decks[(size_t) d].stemFinished())
        {
            switch (a.atEnd)
            {
                case ezarr::EndBehaviour::autoAdvance:
                    if (setlistPos + 1 < (int) setlist.size()) { nextSong(); return; }
                    transportRunning = false;
                    break;
                case ezarr::EndBehaviour::cueNext:
                    transportRunning = false;
                    if (setlistPos + 1 < (int) setlist.size()) selectSong (setlistPos + 1);
                    break;
                case ezarr::EndBehaviour::stop:
                    transportRunning = false;
                    break;
            }
            endCountInIfAny();
        }
    }

    // Milestone 13: General-tab settings backed by real, already-built
    // behavior (PRD §14's own "every switch takes effect immediately")
    // rather than exposing a toggle with nothing behind it yet. "Show all 8
    // decks"/"Floating Pads/FX panels"/"Start pads on the bar"/"Auto-load
    // default pack" are deliberately NOT exposed here -- none has real
    // behavior built yet (documented in this milestone's own port-status
    // note), and a switch with no effect would violate that same criterion.
    bool metronomeEnabled { false };   // owner #11: OFF by default at launch

    // Phase 1.1 P1: single global "is the transport actually running" gate,
    // decoupled from Layer::enabled (which stays a pure per-layer live-mute
    // toggle -- arming a layer no longer makes noise by itself). Read by the
    // audio thread's getNextAudioBlock() (below) to decide whether to call
    // session.renderPerTab() at all this block; written only from the UI
    // thread (Play button / A1-A4 triggers), so a relaxed atomic is enough --
    // no ordering with any other field is required.
    std::atomic<bool> transportRunning { false };
    std::unique_ptr<juce::DialogWindow> settingsWindow;   // kept alive only while the modal is open

    // Milestone 14: single source of truth for "what does this action do,"
    // shared by mouse clicks (already-existing button callbacks reused
    // as-is), keyboard (keyPressed() below), and MIDI.
    ezaction::ActionRegistry actionRegistry;
    ezaction::KeyBindingMap  keyBindings;
    std::unique_ptr<ezaction::MidiActionRouter> midiRouter;
    juce::StringArray openMidiDeviceNames;
    std::vector<std::unique_ptr<juce::MidiInput>> openMidiInputs;   // keeps each device open; destructor stops it

    std::vector<float> scratchR;

    // SPEC_OUTPUT_ROUTING.md: one write-pointer slot per output pair the
    // device currently exposes -- sized in prepareToPlay(), refreshed
    // (never resized) in getNextAudioBlock(). See both methods' own
    // comments.
    std::vector<float*> outputPairPtrsL, outputPairPtrsR;

    std::array<juce::StringArray, kNumDecks> layerNames, layerStatus;

    // Milestone 12: full file paths, tracked alongside layerNames/layerStatus's
    // own per-slot-array pattern -- needed so captureSnapshot() can persist
    // WHICH file each loaded layer/pad/FX slot came from (layerNames only
    // ever stored the display filename, not a path `loadLayer` could reopen).
    std::array<std::array<juce::String, ezdeck::kNumLayers>, kNumDecks> layerFilePaths;

    // Phase 1.1 P1: "Rename Deck" (per-stem rename, in the brief's own
    // terminology -- see layerDisplayName()'s comment for the Row/Deck <->
    // Session slot/Layer mapping this whole task family uses) -- a pure
    // display override, independent of the loaded file's own name so
    // renaming never touches the file on disk. Empty = no override, fall
    // back to the loaded file's name (layerNames), matching every prior
    // caller's existing behavior exactly.
    std::array<std::array<juce::String, ezdeck::kNumLayers>, kNumDecks> layerNameOverride;

    // SPEC_PERFORM_V2 GROUP B: per-slot colour, the exact same "0/empty =
    // no override" pattern rowColourSet/rowColourArgb below already use,
    // just indexed per-layer instead of per-row -- reuses DeckCard's
    // existing setAccent() (falls back to columnAccent() when unset, see
    // refreshSlotLabels()). Deliberately named/shaped like the row-level
    // pair rather than inventing a different convention for the same idea.
    std::array<std::array<bool,         ezdeck::kNumLayers>, kNumDecks> layerColourSet  {};
    std::array<std::array<juce::uint32, ezdeck::kNumLayers>, kNumDecks> layerColourArgb {};

    // SPEC_PERFORM_V2 GROUP H4: which pad (0-11, -1 = none) each layer is
    // linked to. Filled with -1 in the constructor -- unlike bool arrays,
    // std::array can't member-initialize every element to a non-zero
    // default the way `{false}` works above, since 0 is a VALID pad index
    // here (Pad 1), not "unset".
    std::array<std::array<int, ezdeck::kNumLayers>, kNumDecks> layerLinkedPad;

    // SPEC_PERFORM_V2 GROUP H4: edge-detection state for the auto-fire
    // logic in timerCallback() -- tracks the previous tick's (activeFlat,
    // transportRunning) pair so a linked pad fires exactly once per genuine
    // transition into "now playing," never every tick (which would
    // restutter the exclusive pad bank 15 times a second).
    int  lastLinkedPadCheckFlat    { -1 };
    bool lastLinkedPadCheckRunning { false };

    // SPEC_PERFORM_V2 GROUP B: "an empty deck's +/tap opens the Library to
    // load a loop into that slot." -1/-1 = no pending target. Set when an
    // empty DeckCard is tapped (which also opens the Library dock);
    // consumed (and reset) by the very next Library card tap, which loads
    // into this slot instead of auditioning -- see
    // loadAssetIntoDeckSlot() and MySamplesTab::onCardTapMaybeLoad.
    int pendingLoadDeck  { -1 };
    int pendingLoadLayer { -1 };

    // SPEC_PERFORM_V2 GROUP H2: Pad/FX equivalent of pendingLoadDeck/Layer
    // above -- same mechanism, one voice slot instead of a deck layer.
    int  pendingLoadVoiceIndex { -1 };
    bool pendingLoadIsPad      { true };

    // Phase 1.1 P1 "Row Management" -- per-row (per flat-deck-index, see
    // layerDisplayName()'s own comment for the Row=deck mapping) metadata.
    // rowColourSet/rowColour follow the same "0/empty = no override" pattern
    // as layerNameOverride above; deckLabel() (below) is the single place
    // that reads rowName, so every existing caller (toasts, trigger cell
    // text, stem editor header) picks up a rename for free.
    std::array<juce::String, kNumDecks> rowName;
    std::array<juce::String, kNumDecks> rowNotes;
    std::array<juce::String, kNumDecks> rowTags;        // comma-separated
    std::array<bool,         kNumDecks> rowColourSet   { false };
    std::array<juce::uint32, kNumDecks> rowColourArgb  { 0 };

    std::array<juce::String, 12> padFilePaths, fxFilePaths;

    // PerformLive UI/UX Design Notes (Studio One reference): pad/FX
    // Name/Colour, same "message-thread-only display metadata, no engine
    // equivalent" shape as layerNameOverride/layerColourSet/layerColourArgb
    // above -- Mute/Solo, by contrast, are real per-voice engine fields now
    // (OneShotVoice::enabled/soloed) since they gate actual audio output;
    // see isPadMuted()/setPadMuted() etc. below rather than a shadow array.
    std::array<juce::String, 12> padNameOverride, fxNameOverride;
    std::array<bool,         12> padColourSet  { false }, fxColourSet  { false };
    std::array<juce::uint32, 12> padColourArgb { 0 },     fxColourArgb { 0 };

    // Milestone 16: which AssetId (if any) each loaded slot was resolved
    // from -- populated by applySnapshot() when a project references one,
    // consumed by captureSnapshot() so re-saving a project preserves the
    // Library reference instead of silently downgrading it to a raw path.
    std::array<std::array<juce::String, ezdeck::kNumLayers>, kNumDecks> layerAssetIds;
    std::array<juce::String, 12> padAssetIds, fxAssetIds;
    ezlibrary::LibraryManager libraryManager;
    // 0.0 = "no file loaded in this deck yet" -- prepareToPlay() maps that
    // to rateRatio 1.0, and loadLayer() pushes the real ratio the moment a
    // file arrives. (Was a stale 2-element {44100,44100} init from when
    // kNumDecks was 2 -- decks beyond the first two silently got ratio 1.0
    // even after loading a 44.1k file on a 48k device.)
    std::array<bool, kNumDecks>   deckRateSet        {};
    std::array<double, kNumDecks> deckFileSampleRate {};

    // Each clip's DETECTED TEMPO, per deck/layer -- read from the file's own
    // embedded tempo tag when it has one, else measured by ezdsp::analyze on
    // load, and correctable by the user in the editor. This is the app's ONE
    // tempo-per-clip value: it drives the editor's grid, Fit 4 Bars, and the
    // warp calibration reference. (Main.cpp-only, mirroring layerNames/
    // layerStatus's per-slot-array pattern rather than adding a field to
    // Deck.h's Layer.)
    //
    // Named taggedBpm internally for continuity with the many call sites that
    // already use it; the UI calls it what it is -- "DETECTED TEMPO".
    std::array<std::array<double, ezdeck::kNumLayers>, kNumDecks> taggedBpm {};

    // clip editor's Preview voice (M3-T9) -- a single, separate one-shot
    // player, additively mixed on top of session.render()'s output; never
    // touches session/Deck, so deck transport is unaffected by design, not
    // just by convention. `active` gates the other three fields exactly like
    // Layer::enabled gates a layer in Deck::render (Deck.h:46): the message
    // thread always sets active=false before changing layer/pos/rateRatio,
    // and only sets it back to true once they're consistent again, so the
    // audio thread never observes a partially-updated voice.
    struct PreviewVoice
    {
        std::atomic<bool>    active { false };
        const ezdeck::Layer* layer  { nullptr };
        double               pos       { 0.0 };
        double               rateRatio { 1.0 };
        double               loopStart { 0.0 };    // roadmap "Loop Preview" -- wrap-back point when loopEnabled
        std::atomic<bool>    loopEnabled { false };
        std::atomic<double>  scrubTarget { -1.0 };   // roadmap "scrubbing" -- pending seek request, -1 = none; only the audio thread writes `pos` itself
    };
    PreviewVoice preview;
    static constexpr float previewGain = 0.5f;   // matches Deck::render's own default masterGain

    // roadmap "Solo Preview" -- see startPreviewSolo()/stopPreview()
    bool previewSoloActive  { false };
    int  previewSoloDeckIdx { -1 };
    std::array<bool, ezdeck::kNumLayers> previewSoloPrevEnabled {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionComponent)
};

//==============================================================================
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (juce::String name)
        : DocumentWindow (name, juce::Colour (0xff141422), DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new SessionComponent(), true);
       #if JUCE_IOS || JUCE_ANDROID
        setFullScreen (true);   // a tablet app owns the screen; there is no window to move or resize
       #else
        setResizable (true, true);
        centreWithSize (getWidth(), getHeight());
       #endif
        setVisible (true);
    }
    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
// Milestone 12: an automated round-trip check of ProjectFile.h's
// toVar()/fromVar()/save/load, run via `EzPlay.exe --selftest-persistence`
// rather than requiring an interactive GUI session -- this environment has
// no interactive display, but ProjectFile.h depends only on juce_core (not
// juce_gui_basics/AudioAppComponent), so its round-trip CAN be exercised
// automatically inside the same, already-proven build target rather than
// only verified by code review, unlike most other JUCE-adjacent UI code in
// this app. Prints PASS/FAIL per field and returns true only if every one
// matched exactly.
static bool runPersistenceSelfTest()
{
    using namespace ezproject;
    bool allOk = true;
    auto check = [&] (bool cond, const char* desc)
    {
        std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", desc);
        if (! cond) allOk = false;
    };

    ProjectSnapshot original;
    original.settings.metronomeEnabled = false;
    original.settings.trackInput[0] = 0;  original.settings.trackStereo[0] = false;   // LIVE 1: In 1
    original.settings.trackInput[2] = 2;  original.settings.trackStereo[2] = true;    // LIVE 3: In 3+4
    original.settings.trackInstrumentId[1] = "VST3-Kontakt-1a2b3c4d-9e8f7a6b";
    original.settings.trackInstrumentState[1] = juce::MemoryBlock ("hello", 5).toBase64Encoding();
    original.settings.trackMidiChannel[1] = 3;
    original.settings.stripOn[6] = true;    // DECK 7
    original.settings.stripOn[9] = true;    // LIVE 2
    original.settings.stripState[6] = juce::MemoryBlock ("<PERFORM_LIVE/>", 15).toBase64Encoding();
    original.settings.onePadAtATime    = false;
    original.settings.meterVisible     = true;
    original.settings.tempoLockEnabled = true;
    original.settings.webGain = 0.35f;
    original.settings.webMute = true;
    original.masterTempoBpm  = 133.5;
    original.viewedSignature = 3;
    original.masterGain      = 0.75f;
    for (int c = 0; c < ProjectSnapshot::kMixerChannels; ++c)
    {
        original.mixerChannels[(size_t) c].gain = 0.07f * (float) (c + 1);
        original.mixerChannels[(size_t) c].mute = (c % 2 == 0);
        original.mixerChannels[(size_t) c].solo = (c == 3);
        original.mixerChannels[(size_t) c].outputRoute = (c + 1) % MixerChannelSnapshot::kMaxOutputRoutes;   // every channel on its own pair, Metro on Out 23/24
    }
    DeckSnapshot deck;
    deck.flatIndex = 12;
    deck.tempoOverrideBpm = 128.0;
    deck.sourceBpm = 74.5;
    deck.stemMode = false;
    deck.clickFile = "C:/stems/song/Click.wav";
    deck.guideFile = "C:/stems/song/Guide.wav";
    deck.meter = "7/8";
    deck.useSongClick = false;
    deck.rowName = "Intro";
    deck.rowColourSet = true;
    deck.rowColourArgb = 0xff7c5cff;
    deck.rowNotes = "big room, keep energy low";
    deck.rowTags = "energy, breakdown";
    // field order: filePath, assetId, regionStart, regionLength, gain, fadeIn, fadeOut, trimmed, enabled
    deck.layers[0] = { "C:/EzPlay/a1.wav", "11111111-1111-1111-1111-111111111111", 4410, 88200, 0.8f, 100, 200, true, false };
    deck.layers[0].nameOverride = "Vox Chop";   // SPEC_PERFORM_V2 GROUP B
    deck.layers[0].colourSet    = true;
    deck.layers[0].colourArgb   = 0xffec4899;
    deck.layers[0].linkedPad    = 5;   // SPEC_PERFORM_V2 GROUP H4
    deck.layers[2] = { "C:/EzPlay/a3.wav", "", 0, 44100, 1.0f, 0, 0, false, true };   // empty assetId -- pre-Milestone-16-style entry
    // Section playback: a sectioned song with every flag exercised once.
    {
        DeckSnapshot::SectionSnapshot s;
        s.name = "Intro";  s.startBar = 0;  s.colourArgb = 0xff00d9ff; deck.sections.push_back (s);
        s.name = "Chorus"; s.startBar = 8;  s.colourArgb = 0;          s.loopOnEntry = true; deck.sections.push_back (s);
        s = {};
        s.name = "Tag";    s.startBar = 24; s.optional = true; s.pauseAfter = true; s.skip = true; s.cue = "Last-Time"; deck.sections.push_back (s);
        deck.arrangementLengthBars = 32;
        deck.endBehaviour = 1;
        deck.countInBars = 2;
        deck.guideClick = true; deck.guideCues = true; deck.cueLeadBars = 4; deck.cueCounts = false;
    }
    original.decks.push_back (deck);
    original.setlist = { 12, 3, 40 };
    original.jumpMode = 2;
    original.pads.push_back ({ 5, "C:/EzPlay/pad6.wav", "22222222-2222-2222-2222-222222222222", true, 0.9f });
    original.fx.push_back   ({ 2, "C:/EzPlay/fx3.wav", "", false, 1.1f });
    original.scenes[3] = { true, "Chorus", 2, 5, 140.0, { true, false, true, false }, {} };
    original.scenes[3].padActive[7] = true;

    const juce::var v = toVar (original);
    ProjectSnapshot restored;
    const bool parsedOk = fromVar (v, restored);
    check (parsedOk, "fromVar() successfully parses toVar()'s own output");

    check (restored.settings.metronomeEnabled == original.settings.metronomeEnabled &&
           restored.settings.onePadAtATime    == original.settings.onePadAtATime &&
           restored.settings.tempoLockEnabled == original.settings.tempoLockEnabled,
           "settings round-trip exactly");
    check (juce::approximatelyEqual (restored.settings.webGain, 0.35f) && restored.settings.webMute == true,
           "the mixer's WEB strip level and mute round-trip");
    check (restored.settings.trackInput == original.settings.trackInput
           && restored.settings.trackStereo == original.settings.trackStereo
           && restored.settings.trackInput[3] == -1,
           "live track inputs round-trip (LIVE 1 = In 1, LIVE 3 = In 3+4 stereo, others none)");
    check (restored.settings.trackInstrumentId == original.settings.trackInstrumentId
           && restored.settings.trackInstrumentState == original.settings.trackInstrumentState
           && restored.settings.trackMidiChannel == original.settings.trackMidiChannel
           && restored.settings.trackInstrumentId[0].isEmpty(),
           "live track instrument id, state and MIDI channel round-trip (LIVE 2), others empty");
    check (restored.settings.stripOn == original.settings.stripOn && restored.settings.stripState == original.settings.stripState
           && ! restored.settings.stripOn[4],
           "PERFORM LIVE channel strip on/off and state round-trip on decks and live tracks (DECK 7, LIVE 2 on)");

    // A project from before the live tracks: a mic on column 7 (its strip on),
    // an instrument on column 8, an 12-entry mixer. The mic and instrument move
    // to LIVE 1/2 with the strip, the columns become plain stem columns, and
    // the mixer's Pads/Fx/Click/Cues keep their own settings.
    {
        juce::var legacy;
        juce::JSON::parse (R"({"version":1,"settings":{
            "liveInputChannel":[-1,-1,-1,-1,-1,-1,3,-1],"liveInputStereo":[false,false,false,false,false,false,false,false],
            "instrumentId":["","","","","","","","VST3-EZkeys-1"],"instrumentState":["","","","","","","","c3RhdGU="],
            "stripOn":[false,false,false,false,false,false,true,false],"stripState":["","","","","","","PHAvPg==",""]},
            "mixerChannels":[{"gain":1},{"gain":1},{"gain":1},{"gain":1},{"gain":1},{"gain":1},{"gain":1},{"gain":1},
                             {"gain":0.25},{"gain":0.5},{"gain":0.75},{"gain":0.9}],
            "decks":[]})", legacy);
        ProjectSnapshot migrated;
        const bool ok = fromVar (legacy, migrated);
        check (ok && migrated.settings.trackInput[0] == 3 && migrated.settings.trackInstrumentId[1] == "VST3-EZkeys-1"
               && migrated.settings.trackInstrumentState[1] == "c3RhdGU=",
               "an older project's column mic and column instrument move onto LIVE 1 and LIVE 2");
        check (migrated.settings.stripOn[8] && migrated.settings.stripState[8] == "PHAvPg==" && ! migrated.settings.stripOn[6],
               "the mic column's channel strip moves with it, and the column's own strip is off");
        check (juce::approximatelyEqual (migrated.mixerChannels[12].gain, 0.25f) && juce::approximatelyEqual (migrated.mixerChannels[15].gain, 0.9f)
               && juce::approximatelyEqual (migrated.mixerChannels[8].gain, 1.0f),
               "a 12-channel mixer's Pads/FX/Click/Cues settings land on their channels after the live tracks");
    }
    check (juce::approximatelyEqual (restored.masterTempoBpm, original.masterTempoBpm), "masterTempoBpm round-trips exactly");
    check (restored.viewedSignature == original.viewedSignature, "viewedSignature round-trips exactly");
    check (juce::approximatelyEqual (restored.masterGain, original.masterGain), "masterGain round-trips exactly");

    bool mixerOk = true;
    for (int c = 0; c < ProjectSnapshot::kMixerChannels; ++c)
    {
        auto& a = original.mixerChannels[(size_t) c];
        auto& b = restored.mixerChannels[(size_t) c];
        if (! juce::approximatelyEqual (a.gain, b.gain) || a.mute != b.mute || a.solo != b.solo || a.outputRoute != b.outputRoute) mixerOk = false;
    }
    check (mixerOk, "all 16 mixer channels' gain/mute/solo/outputRoute round-trip exactly (Decks 1-8, LIVE 1-4, Pads, FX, Click, Cues)");
    check (restored.mixerChannels[10].outputRoute == 11, "the highest route (Out 23/24) survives the save, not clamped down");

    check (restored.decks.size() == 1 && restored.decks[0].flatIndex == 12
           && juce::approximatelyEqual (restored.decks[0].tempoOverrideBpm, 128.0),
           "deck snapshot's flatIndex/tempoOverrideBpm round-trip exactly");
    check (juce::approximatelyEqual (restored.decks[0].sourceBpm, 74.5) && restored.decks[0].stemMode == false,
           "deck snapshot's original tempo and loop/stem mode round-trip");
    check (restored.decks[0].clickFile == "C:/stems/song/Click.wav" && restored.decks[0].guideFile == "C:/stems/song/Guide.wav"
           && restored.decks[0].useSongClick == false && restored.decks[0].useSongGuide == true,
           "deck snapshot's song click/guide tracks and their built-in/song choice round-trip");
    check (restored.decks[0].meter == "7/8", "a song's own time signature round-trips");
    // Phase 1.1 P1 "Row Management" fields.
    check (restored.decks.size() == 1 && restored.decks[0].sections.size() == 3
           && restored.decks[0].sections[0].name == "Intro" && restored.decks[0].sections[0].startBar == 0
           && restored.decks[0].sections[0].colourArgb == 0xff00d9ff
           && restored.decks[0].sections[1].startBar == 8 && restored.decks[0].sections[1].loopOnEntry
           && restored.decks[0].sections[2].optional && restored.decks[0].sections[2].pauseAfter && restored.decks[0].sections[2].skip,
           "sections round-trip with their names, bars, colours and every flag");
    check (restored.decks.size() == 1 && restored.decks[0].arrangementLengthBars == 32
           && restored.decks[0].endBehaviour == 1 && restored.decks[0].countInBars == 2,
           "arrangement length, end behaviour and count-in round-trip");
    check (restored.setlist == std::vector<int> { 12, 3, 40 } && restored.jumpMode == 2,
           "setlist order and jump mode round-trip");
    check (restored.decks.size() == 1 && restored.decks[0].guideClick && restored.decks[0].guideCues
           && restored.decks[0].cueLeadBars == 4 && ! restored.decks[0].cueCounts
           && restored.decks[0].sections.size() == 3 && restored.decks[0].sections[2].cue == "Last-Time"
           && restored.decks[0].sections[0].cue.isEmpty(),
           "native click/cues settings and a section's chosen cue round-trip");
    {
        // A pre-section project has none of these fields: it must load as
        // "no sections, no setlist, default jump mode", never as garbage.
        ProjectSnapshot legacy;
        auto lv = toVar (legacy);
        if (auto* root = lv.getDynamicObject())
        {
            root->removeProperty ("setlist"); root->removeProperty ("jumpMode");
            if (auto* decks = root->getProperty ("decks").getArray()) decks->clear();
        }
        ProjectSnapshot fromLegacy;
        check (fromVar (lv, fromLegacy) && fromLegacy.setlist.empty() && fromLegacy.jumpMode == 1,
               "a project saved before section playback loads with an empty setlist and the default jump mode");
    }
    check (restored.decks.size() == 1 &&
           restored.decks[0].rowName == "Intro" &&
           restored.decks[0].rowColourSet == true &&
           restored.decks[0].rowColourArgb == 0xff7c5cff &&
           restored.decks[0].rowNotes == "big room, keep energy low" &&
           restored.decks[0].rowTags == "energy, breakdown",
           "deck snapshot's rowName/rowColour/rowNotes/rowTags round-trip exactly");
    check (restored.decks.size() == 1 &&
           restored.decks[0].layers[0].filePath == "C:/EzPlay/a1.wav" &&
           restored.decks[0].layers[0].regionLength == 88200 &&
           juce::approximatelyEqual (restored.decks[0].layers[0].gain, 0.8f) &&
           restored.decks[0].layers[0].fadeInSamples == 100 &&
           restored.decks[0].layers[0].trimmed == true &&
           restored.decks[0].layers[0].enabled == false,
           "a fully-populated layer snapshot round-trips every field exactly");
    // Owner's start marker (Deck.h Layer::regionStart) -- additive field.
    check (restored.decks.size() == 1 &&
           restored.decks[0].layers[0].regionStart == 4410 &&
           restored.decks[0].layers[2].regionStart == 0,
           "layer regionStart (the start marker) round-trips, and defaults to 0");
    // SPEC_PERFORM_V2 GROUP B: name/colour are slot-persistent.
    check (restored.decks.size() == 1 &&
           restored.decks[0].layers[0].nameOverride == "Vox Chop" &&
           restored.decks[0].layers[0].colourSet == true &&
           restored.decks[0].layers[0].colourArgb == 0xffec4899,
           "a layer's nameOverride/colourSet/colourArgb round-trip exactly");
    check (restored.decks.size() == 1 && restored.decks[0].layers[1].filePath.isEmpty(),
           "an untouched layer slot (never assigned) round-trips as empty, not garbage");
    // SPEC_PERFORM_V2 GROUP H4: a linked layer's pad index round-trips, and
    // an UNLINKED layer round-trips as -1 (not 0, which would misread as
    // "linked to Pad 1" -- the exact hasProperty backward-compat hazard
    // layerFromVar()'s own comment explains).
    check (restored.decks.size() == 1 && restored.decks[0].layers[0].linkedPad == 5,
           "a layer's linkedPad round-trips exactly when set");
    check (restored.decks.size() == 1 && restored.decks[0].layers[1].linkedPad == -1,
           "an untouched layer's linkedPad round-trips as -1 (no link), not 0 (Pad 1)");
    // Milestone 16: assetId round-trips when present, and an empty assetId
    // (every project saved before this milestone) stays empty, not garbage.
    check (restored.decks.size() == 1 && restored.decks[0].layers[0].assetId == "11111111-1111-1111-1111-111111111111",
           "a layer's assetId round-trips exactly when present");
    check (restored.decks.size() == 1 && restored.decks[0].layers[2].assetId.isEmpty(),
           "a layer saved with no assetId (pre-Milestone-16-style) round-trips as empty, not garbage");

    check (restored.pads.size() == 1 && restored.pads[0].index == 5 &&
           restored.pads[0].filePath == "C:/EzPlay/pad6.wav" && restored.pads[0].loop == true &&
           restored.pads[0].assetId == "22222222-2222-2222-2222-222222222222",
           "pad snapshot round-trips exactly, including its assetId");
    check (restored.fx.size() == 1 && restored.fx[0].index == 2 &&
           restored.fx[0].filePath == "C:/EzPlay/fx3.wav" && restored.fx[0].loop == false &&
           restored.fx[0].assetId.isEmpty(),
           "FX snapshot round-trips exactly, with an empty assetId staying empty");

    auto& s = restored.scenes[3];
    check (s.filled && s.name == "Chorus" && s.signatureIndex == 2 && s.activeSlot == 5
           && juce::approximatelyEqual (s.bpm, 140.0) && s.tabEnabled[0] && ! s.tabEnabled[1] && s.padActive[7],
           "a fully-populated scene snapshot round-trips every field exactly");
    check (! restored.scenes[0].filled, "an untouched scene slot round-trips as unfilled, not garbage");

    // file round-trip
    auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ezplay_selftest_project.json");
    check (saveToFile (original, tempFile), "saveToFile() succeeds");
    ProjectSnapshot fromDisk;
    check (loadFromFile (tempFile, fromDisk), "loadFromFile() succeeds reading back what saveToFile() just wrote");
    check (fromDisk.decks.size() == 1 && fromDisk.decks[0].flatIndex == 12, "a full file round-trip (not just the in-memory var) reproduces deck data exactly");
    tempFile.deleteFile();

    // version mismatch handling
    ProjectSnapshot versioned = original;
    versioned.version = 999;
    juce::var badVersion = toVar (versioned);
    ProjectSnapshot shouldFail;
    const bool rejectedBadVersion = ! fromVar (badVersion, shouldFail);
    check (rejectedBadVersion, "fromVar() cleanly rejects a version mismatch rather than guessing at a migration");

    check (! loadFromFile (juce::File ("C:/EzPlay/does_not_exist_12345.json"), fromDisk),
           "loadFromFile() cleanly returns false for a nonexistent file, not a crash");

    // Milestone 16: a hand-written, pre-Milestone-16-style project JSON --
    // no "assetId" key anywhere, not even an empty one -- must still parse
    // cleanly. This is the real backward-compatibility guarantee (not just
    // "an empty string round-trips"), proven against a literal old-format
    // document rather than one this codebase's own current toVar() produced.
    {
        const char* oldStyleJson =
            "{\"version\":1,\"settings\":{\"metronomeEnabled\":true,\"onePadAtATime\":true,"
            "\"meterVisible\":true,\"tempoLockEnabled\":false},\"masterTempoBpm\":120.0,"
            "\"viewedSignature\":0,\"masterGain\":1.0,\"mixerChannels\":[],"
            "\"decks\":[{\"flatIndex\":3,\"tempoOverrideBpm\":-1.0,\"layers\":"
            "[{\"filePath\":\"C:/EzPlay/a1.wav\",\"regionLength\":100,\"gain\":1.0,"
            "\"fadeInSamples\":0,\"fadeOutSamples\":0,\"trimmed\":false,\"enabled\":true}]}],"
            "\"pads\":[],\"fx\":[],\"scenes\":[]}";
        juce::var parsedOld;
        const bool oldJsonParsedOk = juce::JSON::parse (juce::String (oldStyleJson), parsedOld).wasOk();
        ProjectSnapshot oldRestored;
        check (oldJsonParsedOk && fromVar (parsedOld, oldRestored), "a hand-written pre-Milestone-16 project (no assetId key anywhere) parses successfully");
        check (oldRestored.decks.size() == 1 && oldRestored.decks[0].layers[0].filePath == "C:/EzPlay/a1.wav"
               && oldRestored.decks[0].layers[0].assetId.isEmpty(),
               "its layer's filePath is preserved and assetId defaults to empty, not garbage or a parse failure");
    }

    // A project saved before PX-B has 7 mixer channels (Tab1-4, Pads, Fx,
    // Metro). Pads/Fx/Metro must land on their own strips (12/13/14, after
    // Decks 5-8 and LIVE 1-4), not on Decks 5-7 -- otherwise opening an old
    // set silently swaps outputs.
    {
        const char* sevenChannelJson =
            "{\"version\":1,\"mixerChannels\":["
            "{\"gain\":1.0,\"outputRoute\":1},{\"gain\":1.0,\"outputRoute\":2},"
            "{\"gain\":1.0,\"outputRoute\":3},{\"gain\":1.0,\"outputRoute\":4},"
            "{\"gain\":0.5,\"outputRoute\":5},{\"gain\":0.25,\"mute\":true,\"outputRoute\":2},"
            "{\"gain\":0.75,\"outputRoute\":99}],"
            "\"decks\":[],\"pads\":[],\"fx\":[],\"scenes\":[]}";
        juce::var parsed;
        ProjectSnapshot legacy;
        const bool ok = juce::JSON::parse (juce::String (sevenChannelJson), parsed).wasOk() && fromVar (parsed, legacy);
        check (ok, "a 7-channel (pre-PX-B) mixer project parses");
        check (legacy.mixerChannels[0].outputRoute == 1 && legacy.mixerChannels[3].outputRoute == 4,
               "old Decks 1-4 keep their outputs");
        check (legacy.mixerChannels[4].outputRoute == 0 && legacy.mixerChannels[7].outputRoute == 0,
               "Decks 5-8 (new) start on Main instead of inheriting Pads/FX/Metro routes");
        check (legacy.mixerChannels[12].outputRoute == 5 && juce::approximatelyEqual (legacy.mixerChannels[12].gain, 0.5f),
               "old Pads channel lands on the Pads strip");
        check (legacy.mixerChannels[13].outputRoute == 2 && legacy.mixerChannels[13].mute,
               "old FX channel lands on the FX strip");
        check (legacy.mixerChannels[14].outputRoute == MixerChannelSnapshot::kMaxOutputRoutes - 1,
               "old Metro channel lands on the Metro strip, and an out-of-range route is clamped");
    }

    return allOk;
}

// Milestone 16: an automated round-trip + resolution-chain check of
// Library.h, run via `EzPlay.exe --selftest-library` for the same reason
// runPersistenceSelfTest() exists -- Library.h depends only on juce_core,
// so its behavior can be verified automatically inside the existing,
// already-proven build target rather than by code review alone.
static bool runLibrarySelfTest()
{
    using namespace ezlibrary;
    bool allOk = true;
    auto check = [&] (bool cond, const char* desc)
    {
        std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", desc);
        if (! cond) allOk = false;
    };

    auto rootDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ezplay_selftest_library");
    rootDir.deleteRecursively();
    rootDir.createDirectory();

    // ---- hashFile() is deterministic and content-sensitive ----
    auto fileA = rootDir.getChildFile ("a.raw");
    auto fileB = rootDir.getChildFile ("b.raw");
    fileA.replaceWithText ("hello world");
    fileB.replaceWithText ("hello world");   // identical content, different file
    auto fileC = rootDir.getChildFile ("c.raw");
    fileC.replaceWithText ("something else");

    const auto hashA1 = Library::hashFile (fileA);
    const auto hashA2 = Library::hashFile (fileA);
    const auto hashB  = Library::hashFile (fileB);
    const auto hashC  = Library::hashFile (fileC);

    check (hashA1.isNotEmpty(), "hashFile() produces a non-empty hash for a real file");
    check (hashA1 == hashA2, "hashFile() is deterministic -- hashing the same file twice gives the same result");
    check (hashA1 == hashB, "hashFile() gives the SAME hash for two different files with IDENTICAL content -- this is what duplicate detection relies on");
    check (hashA1 != hashC, "hashFile() gives a DIFFERENT hash for files with different content");

    // b.raw/c.raw were only needed for the 4 checks above -- removed now so
    // later blocks (which deliberately give a.raw UNIQUE content) can't be
    // confused by b.raw's deliberately-colliding content.
    fileB.deleteFile();
    fileC.deleteFile();

    // ---- Library load()/save()/upsert() round-trip ----
    {
        Library lib (rootDir);
        check (! lib.load(), "load() on a library with no library.json yet returns false (a fresh library, not an error)");

        LibraryEntry entry;
        entry.assetId      = juce::Uuid().toString();
        entry.contentHash  = hashA1;
        entry.relativePath = "a.raw";
        entry.name         = "Test Asset";
        entry.tags         = "kick,drum";
        entry.category     = "Drums";
        entry.detectedBpm  = 128.0;
        entry.favorite     = true;
        entry.importedAtMs = 1234567;
        entry.artist        = "Test Artist";
        entry.album         = "Test Album";
        entry.key           = "Am";
        entry.timeSignature = "4/4";
        entry.genre         = "House";
        entry.notes         = "great for breakdowns";
        entry.creator       = "Test Creator";
        entry.rating        = 4;
        entry.collectionId  = "collection-42";
        lib.upsert (entry);
        check (lib.save(), "save() succeeds");

        Library reloaded (rootDir);
        check (reloaded.load(), "load() succeeds after save()");
        auto found = reloaded.findById (entry.assetId);
        check (found.has_value(), "findById() finds the saved entry after a fresh load");
        check (found.has_value() && found->contentHash == hashA1 && found->name == "Test Asset"
               && found->tags == "kick,drum" && found->category == "Drums"
               && juce::approximatelyEqual (found->detectedBpm, 128.0) && found->favorite
               && found->importedAtMs == 1234567,
               "every LibraryEntry field round-trips exactly through save()/load()");
        // Phase 1.1 P2 "Metadata editing" fields.
        check (found.has_value() && found->artist == "Test Artist" && found->album == "Test Album"
               && found->key == "Am" && found->timeSignature == "4/4" && found->genre == "House"
               && found->notes == "great for breakdowns" && found->creator == "Test Creator"
               && found->rating == 4,
               "artist/album/key/timeSignature/genre/notes/creator/rating round-trip exactly");
        check (found.has_value() && found->collectionId == "collection-42",
               "collectionId round-trips exactly");
    }

    // ---- resolve() (step 1: stored path) ----
    {
        Library lib (rootDir);
        lib.load();
        auto found = lib.findById (lib.entries()[0].assetId);
        auto resolved = lib.resolve (found->assetId);
        check (resolved.existsAsFile() && resolved.getFileName() == "a.raw",
               "resolve() finds the asset via its stored relative path");
    }

    // ---- resolve() step 2: rescanAndRepair() finds a file moved/renamed OUTSIDE the app ----
    {
        Library lib (rootDir);
        lib.load();
        const auto assetId = lib.entries()[0].assetId;

        // simulate an external rename: move a.raw -> renamed.raw without going through the Library at all
        auto renamed = rootDir.getChildFile ("renamed.raw");
        fileA.moveFileTo (renamed);

        auto brokenResolve = lib.resolve (assetId);
        check (! brokenResolve.existsAsFile(), "resolve() correctly reports missing once the file has moved outside the app's knowledge");

        const int repaired = lib.rescanAndRepair();
        check (repaired == 1, "rescanAndRepair() repairs exactly the one entry whose file moved");

        auto fixedResolve = lib.resolve (assetId);
        check (fixedResolve.existsAsFile() && fixedResolve.getFileName() == "renamed.raw",
               "resolve() now finds the asset at its new location, via content-hash matching -- the SAME AssetId throughout, never reissued");

        // the repair must have been persisted, not just held in memory
        Library reloaded (rootDir);
        reloaded.load();
        check (reloaded.resolve (assetId).existsAsFile(), "the repaired path was actually saved to library.json, not just fixed in memory");
    }

    // ---- resolve() for a truly missing asset (no file anywhere with a matching hash) ----
    {
        Library lib (rootDir);
        lib.load();
        LibraryEntry ghost;
        ghost.assetId = juce::Uuid().toString();
        ghost.contentHash = "0000000000000000000000000000000000000000000000000000000000000000";
        ghost.relativePath = "does_not_exist.raw";
        lib.upsert (ghost);
        lib.save();

        check (! lib.resolve (ghost.assetId).existsAsFile(), "resolve() cleanly reports missing for an asset with no matching file anywhere");
        lib.rescanAndRepair();
        check (! lib.resolve (ghost.assetId).existsAsFile(), "rescanAndRepair() does not fabricate a match when none exists");
    }

    // ---- LibraryManager: multi-root aggregation, no duplicate roots, global AssetId uniqueness ----
    {
        auto rootDir2 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ezplay_selftest_library_2");
        rootDir2.deleteRecursively();
        rootDir2.createDirectory();
        auto fileD = rootDir2.getChildFile ("d.raw");
        fileD.replaceWithText ("second root content");

        LibraryEntry entry2;
        entry2.assetId = juce::Uuid().toString();
        entry2.contentHash = Library::hashFile (fileD);
        entry2.relativePath = "d.raw";
        {
            Library lib2 (rootDir2);
            lib2.upsert (entry2);
            lib2.save();
        }

        LibraryManager mgr;
        auto& r1a = mgr.addRoot (rootDir);
        auto& r1b = mgr.addRoot (rootDir);   // same path again
        check (&r1a == &r1b, "addRoot() with the same path twice returns the SAME Library instance, not a duplicate");

        mgr.addRoot (rootDir2);
        check (mgr.libraries().size() == 2, "two distinct roots produce exactly two Library instances");

        auto resolvedFromRoot2 = mgr.resolve (entry2.assetId);
        check (resolvedFromRoot2.existsAsFile() && resolvedFromRoot2.getFileName() == "d.raw",
               "LibraryManager::resolve() finds an asset that only exists in the SECOND root");

        rootDir2.deleteRecursively();
    }

    // ---- Folders: saved with the library, old stem-set groups become folders, pictures stay inside the library ----
    {
        auto folderRoot = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ezplay_selftest_folders");
        folderRoot.deleteRecursively();
        folderRoot.createDirectory();

        LibraryEntry drums, bass, kick;
        drums.assetId = juce::Uuid().toString(); drums.name = "Grace - Drums";
        bass.assetId  = juce::Uuid().toString(); bass.name  = "Grace - Bass";
        kick.assetId  = juce::Uuid().toString(); kick.name  = "Old Kick"; kick.collectionId = "legacy-group";

        juce::StringArray pairNames, pairIds;
        pairNames.add (drums.name); pairNames.add (bass.name);
        pairIds.add (drums.assetId); pairIds.add (bass.assetId);

        juce::String folderId;
        {
            Library lib (folderRoot);
            lib.upsert (drums); lib.upsert (bass); lib.upsert (kick);
            check (Library::suggestFolderName (pairNames) == "Grace",
                   "suggestFolderName() uses the samples' shared leading words");
            folderId = lib.createFolder ("Grace", pairIds).id;
            lib.save();
        }
        {
            Library lib (folderRoot);
            lib.load();
            auto folder = lib.findFolder (folderId);
            check (folder.has_value() && folder->name == "Grace" && lib.folderSize (folderId) == 2,
                   "a folder and its members survive save + load");
            check (lib.findFolder ("legacy-group").has_value() && lib.folderSize ("legacy-group") == 1,
                   "an older stem-set group (a collectionId with no folder record) loads as a folder");

            juce::Image big (juce::Image::RGB, 900, 600, true);
            { juce::Graphics g (big); g.fillAll (juce::Colours::orange); }
            auto source = folderRoot.getSiblingFile ("ezplay_selftest_folder_picture.png");
            source.deleteFile();
            { juce::FileOutputStream out (source); juce::PNGImageFormat().writeImageToStream (big, out); }
            juce::String error;
            check (lib.setFolderImage (folderId, source, error)
                     && lib.folderImageFile (folderId).isAChildOf (folderRoot.getChildFile ("Folders")),
                   "setFolderImage() keeps its own copy of the picture inside the library's Folders directory");
            const auto stored = juce::ImageFileFormat::loadFrom (lib.folderImageFile (folderId));
            check (stored.isValid() && stored.getWidth() <= 512 && stored.getHeight() <= 512,
                   "a large folder picture is scaled down to 512 px");
            source.deleteFile();

            auto tampered = *lib.findFolder (folderId);
            tampered.imagePath = "../../outside.png";
            lib.upsertFolder (tampered);
            check (lib.folderImageFile (folderId) == juce::File(),
                   "a folder picture path that leads outside the library is ignored");

            lib.removeFolder (folderId);
            check (! lib.findFolder (folderId).has_value() && lib.findById (drums.assetId).has_value()
                     && lib.findById (drums.assetId)->collectionId.isEmpty(),
                   "deleting a folder keeps its samples and takes them out of it");
        }
        folderRoot.deleteRecursively();
    }

    // ---- resolveAssetOrPath(): the shared helper every load boundary (Deck layers, Pads, FX) uses ----
    {
        LibraryManager mgr;
        auto& lib = mgr.addRoot (rootDir);
        auto entry = lib.entries()[0];   // the "Test Asset" entry from the round-trip block above, now at renamed.raw

        auto resolvedViaAsset = resolveAssetOrPath (mgr, entry.assetId, "C:/some/stale/path/that/does/not/exist.wav");
        check (resolvedViaAsset.existsAsFile() && resolvedViaAsset.getFileName() == "renamed.raw",
               "resolveAssetOrPath() prefers the Library resolution over a stale raw path when the assetId resolves successfully");

        auto resolvedViaFallback = resolveAssetOrPath (mgr, juce::String(), rootDir.getChildFile ("renamed.raw").getFullPathName());
        check (resolvedViaFallback.existsAsFile() && resolvedViaFallback.getFileName() == "renamed.raw",
               "resolveAssetOrPath() falls back to the raw path when assetId is empty -- exact pre-Milestone-16 behavior for every existing project file");

        auto resolvedMissingBoth = resolveAssetOrPath (mgr, juce::String ("00000000-0000-0000-0000-000000000000"), "C:/definitely/missing.wav");
        check (! resolvedMissingBoth.existsAsFile(),
               "resolveAssetOrPath() returns a non-existent File (not a crash) when neither the assetId nor the raw path resolves");
    }

    rootDir.deleteRecursively();
    return allOk;
}

// Milestone 16-T2: verifies eximport::importFile() -- hashing/dedup, real
// audio decoding via juce::AudioFormatManager (the same one loadLayer() uses),
// copying into the library's Samples/ folder, and metadata (BPM) extraction
// via ezdsp::analyze(). Uses a real WAV file (written with juce::WavAudioFormat)
// so the decode path is genuinely exercised, not just a hash of raw text bytes.
static bool runImporterSelfTest()
{
    using namespace ezlibrary;
    bool allOk = true;
    auto check = [&] (bool cond, const char* desc)
    {
        std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", desc);
        if (! cond) allOk = false;
    };

    auto rootDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ezplay_selftest_importer");
    rootDir.deleteRecursively();
    rootDir.createDirectory();
    auto sourceDir = rootDir.getChildFile ("Source");
    sourceDir.createDirectory();

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    // ---- write a short, real WAV file (a few cycles of a sine wave) ----
    auto wavFile = sourceDir.getChildFile ("tone.wav");
    {
        constexpr double sr = 44100.0;
        constexpr int numSamples = 4410;   // 0.1s
        juce::AudioBuffer<float> buf (1, numSamples);
        auto* d = buf.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i)
            d[i] = (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) i / sr) * 0.5);

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::FileOutputStream> stream (wavFile.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (stream.get(), sr, 1, 16, {}, 0));
        if (writer != nullptr)
        {
            stream.release();   // writer now owns the stream
            writer->writeFromAudioSampleBuffer (buf, 0, numSamples);
        }
    }
    check (wavFile.existsAsFile(), "test fixture: a real WAV file was written to disk");

    auto libRoot = rootDir.getChildFile ("Library");
    libRoot.createDirectory();
    Library library (libRoot);

    // ---- importing a nonexistent file fails cleanly ----
    {
        auto missingResult = eximport::importFile (library, sourceDir.getChildFile ("does_not_exist.wav"), formatManager);
        check (! missingResult.success, "importFile() fails cleanly for a nonexistent source file");
        check (missingResult.errorMessage.isNotEmpty(), "importFile() reports a non-empty error message for a missing file");
    }

    // ---- importing an unrecognized format fails cleanly (not a crash, no entry created) ----
    {
        auto junkFile = sourceDir.getChildFile ("not_audio.txt");
        junkFile.replaceWithText ("this is not an audio file");
        auto junkResult = eximport::importFile (library, junkFile, formatManager);
        check (! junkResult.success, "importFile() fails cleanly for a file the AudioFormatManager can't decode");
        check (library.entries().empty(), "a failed import registers no LibraryEntry");
    }

    // ---- importing a real WAV succeeds: references it in place (default), extracts BPM ----
    eximport::ImportResult firstImport;
    {
        firstImport = eximport::importFile (library, wavFile, formatManager, "Tone", "test,sine");
        check (firstImport.success, "importFile() succeeds for a real, decodable WAV file");
        check (! firstImport.wasDuplicate, "the first import of a new file is not flagged as a duplicate");
        check (firstImport.entry.isValid(), "importFile() returns a valid LibraryEntry (non-empty assetId)");
        check (firstImport.entry.category == "Tone" && firstImport.entry.tags == "test,sine",
               "importFile() preserves the caller-supplied category/tags");
        check (firstImport.entry.detectedBpm > 0.0, "importFile() extracts a detected BPM via ezdsp::analyze()");

        // Bug report: default import no longer copies -- it references the
        // original file in place, and nothing new appears in Samples/.
        check (firstImport.entry.external, "by default, importFile() registers the entry as external (no copy)");
        check (firstImport.entry.externalPath == wavFile.getFullPathName(),
               "an external entry's externalPath points at the ORIGINAL source file");
        check (! library.getRoot().getChildFile ("Samples").isDirectory(),
               "a default (external) import creates no Samples/ folder at all -- nothing was copied");
        check (library.resolve (firstImport.entry.assetId).getFullPathName() == wavFile.getFullPathName(),
               "Library::resolve() on an external entry returns the original file directly");

        check (library.findById (firstImport.entry.assetId).has_value(),
               "the new LibraryEntry is persisted and findable immediately after import");
    }

    // ---- copyIntoLibrary == true reproduces the original copy-on-import behavior ----
    {
        // A distinct, genuinely different-content file (not just a different
        // name) so this isn't deduped against firstImport by content hash --
        // this block wants a FRESH entry to check the copy path itself.
        auto copyOnImportFile = sourceDir.getChildFile ("copy_on_import.wav");
        {
            juce::AudioBuffer<float> buf (1, 2205);
            auto* d = buf.getWritePointer (0);
            for (int i = 0; i < 2205; ++i)
                d[i] = (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 880.0 * (double) i / 44100.0) * 0.5);
            juce::WavAudioFormat wavFormat;
            std::unique_ptr<juce::FileOutputStream> stream (copyOnImportFile.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (stream.get(), 44100.0, 1, 16, {}, 0));
            if (writer != nullptr)
            {
                stream.release();   // writer now owns the stream
                writer->writeFromAudioSampleBuffer (buf, 0, 2205);
            }
            // writer/stream destroyed here, at scope exit -- finalizes the
            // WAV header/flushes to disk BEFORE importFile() below reads it
            // (same reason the fixture-writing block above ends its own
            // scope before this test's very first check()).
        }

        auto copiedImport = eximport::importFile (library, copyOnImportFile, formatManager, {}, {}, true);
        check (copiedImport.success, "importFile(copyIntoLibrary=true) succeeds");
        check (! copiedImport.entry.external, "copyIntoLibrary=true registers a non-external entry");
        auto copiedFile = libRoot.getChildFile (copiedImport.entry.relativePath);
        check (copiedFile.existsAsFile(), "copyIntoLibrary=true copies the source file into the library's own Samples/ folder");
        check (copiedFile.getFullPathName() != copyOnImportFile.getFullPathName(),
               "the copied file is a distinct file from the original source, not a reference to it");

        // ---- copyExternalEntryIntoLibrary() performs the SAME copy, deferred ----
        auto deferredEntry = firstImport.entry;   // still external, from the block above
        const bool copyOk = eximport::copyExternalEntryIntoLibrary (library, deferredEntry);
        check (copyOk, "copyExternalEntryIntoLibrary() succeeds for an external entry whose source file still exists");
        check (! deferredEntry.external, "copyExternalEntryIntoLibrary() flips the entry to non-external");
        auto deferredCopiedFile = libRoot.getChildFile (deferredEntry.relativePath);
        check (deferredCopiedFile.existsAsFile(), "copyExternalEntryIntoLibrary() actually copied the file into Samples/");
        check (library.findById (deferredEntry.assetId)->external == false,
               "copyExternalEntryIntoLibrary() persists the flipped entry back into the library");
    }

    // ---- importing the SAME file again is recognized as a duplicate: no second copy, no second entry ----
    {
        const int entryCountBefore = (int) library.entries().size();
        auto secondImport = eximport::importFile (library, wavFile, formatManager);
        check (secondImport.success, "re-importing the same file still reports success");
        check (secondImport.wasDuplicate, "re-importing the same file is recognized as a duplicate via content hash");
        check (secondImport.entry.assetId == firstImport.entry.assetId,
               "the duplicate import returns the SAME AssetId as the original -- no new identity is minted");
        check ((int) library.entries().size() == entryCountBefore,
               "importing a duplicate does not add a second LibraryEntry");
    }

    // ---- importing a byte-identical copy under a different filename is ALSO recognized as a duplicate ----
    {
        auto renamedCopy = sourceDir.getChildFile ("tone_copy.wav");
        wavFile.copyFileTo (renamedCopy);
        auto copyImport = eximport::importFile (library, renamedCopy, formatManager);
        check (copyImport.wasDuplicate && copyImport.entry.assetId == firstImport.entry.assetId,
               "content-hash dedup recognizes a byte-identical file even under a different filename");
    }

    // ---- the background-import split (analyzeImportFile + registerAnalyzedImport)
    //      behaves identically to importFile(copyIntoLibrary=false) ----
    {
        auto analyzed = eximport::analyzeImportFile (wavFile, formatManager);
        check (analyzed.readable, "analyzeImportFile() succeeds for a real, decodable WAV file");
        check (analyzed.contentHash == firstImport.entry.contentHash,
               "analyzeImportFile() computes the same content hash importFile() did");
        check (analyzed.detectedBpm > 0.0, "analyzeImportFile() extracts a detected BPM without touching the Library");

        // registering an already-imported hash is a duplicate, not a new entry
        const int entryCountBefore = (int) library.entries().size();
        auto reg = eximport::registerAnalyzedImport (library, analyzed);
        check (reg.success && reg.wasDuplicate && reg.entry.assetId == firstImport.entry.assetId,
               "registerAnalyzedImport() dedups by content hash exactly like importFile()");
        check ((int) library.entries().size() == entryCountBefore,
               "a duplicate registerAnalyzedImport() adds no second LibraryEntry");

        // a genuinely new file registers as a fresh external entry
        auto splitFile = sourceDir.getChildFile ("split_import.wav");
        {
            juce::AudioBuffer<float> buf (1, 2205);
            auto* d = buf.getWritePointer (0);
            for (int i = 0; i < 2205; ++i)
                d[i] = (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 660.0 * (double) i / 44100.0) * 0.5);
            juce::WavAudioFormat wavFormat;
            std::unique_ptr<juce::FileOutputStream> stream (splitFile.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (stream.get(), 44100.0, 1, 16, {}, 0));
            if (writer != nullptr)
            {
                stream.release();
                writer->writeFromAudioSampleBuffer (buf, 0, 2205);
            }
        }
        auto analyzedNew = eximport::analyzeImportFile (splitFile, formatManager);
        auto regNew      = eximport::registerAnalyzedImport (library, analyzedNew, "Split", "bg");
        check (regNew.success && ! regNew.wasDuplicate, "registerAnalyzedImport() creates a fresh entry for a new file");
        check (regNew.entry.external && regNew.entry.externalPath == splitFile.getFullPathName(),
               "the split path registers external (reference-in-place), same as importFile()'s default");
        check (regNew.entry.category == "Split" && regNew.entry.tags == "bg",
               "registerAnalyzedImport() preserves caller-supplied category/tags");

        // an unreadable input fails cleanly through the split too
        auto junkAnalyzed = eximport::analyzeImportFile (sourceDir.getChildFile ("missing.wav"), formatManager);
        check (! junkAnalyzed.readable, "analyzeImportFile() fails cleanly for a missing file");
        auto junkReg = eximport::registerAnalyzedImport (library, junkAnalyzed);
        check (! junkReg.success, "registerAnalyzedImport() propagates an analysis failure instead of registering junk");
    }

    rootDir.deleteRecursively();
    return allOk;
}

#if JUCE_WINDOWS
 // The crash report names the exception and the module it happened in, which
 // needs the Win32 types. Included here, after the app's own code, so its
 // macros cannot reach anything above.
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

// ============================================================================
//  Crash safety net. Owner: "we need a very strong app that won't fail." When
//  it does fail anyway, the failure must be visible and reportable rather
//  than a window that silently vanishes mid-service: the handler writes a
//  report (version, time, stack) beside the log, and the next launch says so
//  and where the file is.
// ============================================================================
static juce::File crashReportFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("EzPlay");
}

static juce::File pendingCrashReportFile()
{
    return crashReportFolder().getChildFile ("PerformLive-crash.txt");
}

static void performliveCrashHandler (void* info)
{
    // Keep this minimal: the process is already broken. No allocations we
    // can avoid, no logger, one file write.
    const auto file = pendingCrashReportFile();
    juce::String report;
    report << "PerformLive closed unexpectedly\n"
           << "Time:    " << juce::Time::getCurrentTime().toString (true, true) << "\n"
           << "Version: " << JUCE_APPLICATION_VERSION_STRING << "\n"
           << "OS:      " << juce::SystemStats::getOperatingSystemName() << "\n";
   #if JUCE_WINDOWS
    // Which exception, and which module it happened in -- a plugin's own
    // DLL, more often than not.
    if (auto* ep = static_cast<PEXCEPTION_POINTERS> (info); ep != nullptr && ep->ExceptionRecord != nullptr)
    {
        const auto* rec = ep->ExceptionRecord;
        report << "Code:    0x" << juce::String::toHexString ((juce::int64) rec->ExceptionCode) << "\n";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR) rec->ExceptionAddress, &mod) && mod != nullptr)
        {
            char path[1024] = {};
            GetModuleFileNameA (mod, path, (DWORD) sizeof (path));
            report << "Module:  " << path << "\n";
        }
        if (rec->ExceptionCode == 0xE06D7363)
            report << "         (a C++ exception thrown by that module and caught by nobody)\n";
    }
   #endif
    report << "\nStack:\n" << juce::SystemStats::getStackBacktrace() << "\n";
    file.getParentDirectory().createDirectory();
    file.replaceWithText (report);
}

class EzPlayApplication : public juce::JUCEApplication
{
public:
    // Rebrand: the user-facing name is PerformLive (window title, single-
    // instance key). Internal names -- class names, log folder, settings
    // folder, file names -- deliberately stay EzPlay so nothing on disk
    // (recent-projects MRU, logs) is orphaned by the rename.
    const juce::String getApplicationName() override    { return "PerformLive"; }
    const juce::String getApplicationVersion() override  { return "0.4.0"; }
    // Session bug report: multiple EzPlay.exe processes running at once were
    // repeatedly found autosaving over the SAME ezplay_project.json/
    // .autosave.json independently of each other, corrupting the project
    // file (fixed ad hoc via taskkill each time). Refusing a second instance
    // outright removes the whole failure class instead of relying on the
    // user to notice and kill stray processes by hand.
    // One instance only -- except the copies we launch ourselves: the
    // plugin-scanner worker (InstrumentHost.h) and the command-line
    // self-tests, which must run while the app is open.
    bool moreThanOneInstanceAllowed() override
    {
        const auto params = getCommandLineParameters();
        return params.contains (ezinst::kScannerProcessUID) || params.contains ("--selftest") || params.contains ("--list-audio-devices")
            || params.contains ("--scan-plugins") || params.contains ("--test-instrument");
    }

    // JUCE's own mechanism for this: when moreThanOneInstanceAllowed() is
    // false, a second launch attempt never runs its own initialise() at all
    // -- JUCE's IPC forwards its command line to the FIRST instance and
    // calls this method there instead. Bring the existing window to front
    // rather than silently swallowing the second launch, so the user sees
    // that their "launch" actually did something.
    void anotherInstanceStarted (const juce::String&) override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setMinimised (false);
            mainWindow->toFront (true);
        }
    }

    void initialise (const juce::String& commandLine) override
    {
        // PX-D: launched by ourselves as the plugin-scanner worker? Then this
        // process only probes plugin files and reports back -- no window, no
        // audio, no project. A plugin that crashes here crashes only here.
        {
            auto scanner = std::make_unique<ezinst::ScannerSubprocess>();
            if (scanner->initialiseFromCommandLine (commandLine, ezinst::kScannerProcessUID))
            {
                scannerSubprocess = std::move (scanner);
                return;
            }
        }

        // PX-D: "PerformLive.exe --test-instrument <name>" loads one scanned
        // instrument in THIS process, prepares it and renders a few blocks
        // with a note held, then quits 0. A crash here is a crash in that
        // plugin only -- run one per process to find which plugins fail.
        if (commandLine.contains ("--test-instrument"))
        {
            const auto wanted = commandLine.fromFirstOccurrenceOf ("--test-instrument", false, false).trim().unquoted().trim();
            ezinst::PluginLibrary lib;
            if (auto* settings = SessionComponent::getAppSettings()) lib.loadFrom (*settings);
            std::optional<juce::PluginDescription> found;
            for (const auto& d : lib.instruments())
                if (wanted.isEmpty() || d.name.containsIgnoreCase (wanted)) { found = d; break; }
            if (! found) { std::printf ("no instrument matching [%s] in the scanned list\n", wanted.toRawUTF8()); setApplicationReturnValue (2); quit(); return; }
            std::printf ("loading %s (%s) from %s\n", found->name.toRawUTF8(), found->pluginFormatName.toRawUTF8(), found->fileOrIdentifier.toRawUTF8());
            std::fflush (stdout);
            juce::SystemStats::setApplicationCrashHandler (performliveCrashHandler);
            try
            {
                juce::String error;
                auto inst = lib.formatManager().createPluginInstance (*found, 48000.0, 512, error);
                if (inst == nullptr) { std::printf ("FAILED to create: %s\n", error.toRawUTF8()); setApplicationReturnValue (3); quit(); return; }
                std::printf ("created: %d in, %d out, buses in/out %d/%d, editor=%d\n", inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels(),
                             inst->getBusCount (true), inst->getBusCount (false), (int) inst->hasEditor());
                std::fflush (stdout);
                ezinst::InstrumentSlot slot;
                slot.prepare (48000.0, 512);
                slot.install (std::move (inst), *found);
                std::printf ("prepared\n"); std::fflush (stdout);
                slot.addMidi (juce::MidiMessage::noteOn (1, 60, 0.8f));
                std::vector<float> l (512), r (512);
                float peak = 0.0f;
                for (int b = 0; b < 40; ++b)
                {
                    slot.render (l.data(), r.data(), 512);
                    for (float v : l) peak = juce::jmax (peak, std::fabs (v));
                }
                slot.addMidi (juce::MidiMessage::noteOff (1, 60));
                slot.render (l.data(), r.data(), 512);
                std::printf ("rendered 40 blocks, peak %.3f%s\n", peak, peak > 0.0f ? "" : " (silent -- a sampler with no patch loaded is expected to be silent)");
                slot.clear(); slot.collectRetired();
                std::printf ("OK %s\n", found->name.toRawUTF8());
            }
            catch (const std::exception& e) { std::printf ("THREW: %s\n", e.what()); setApplicationReturnValue (4); }
            catch (...)                     { std::printf ("THREW (non-std)\n"); setApplicationReturnValue (4); }
            std::fflush (stdout);
            quit();
            return;
        }

        // PX-D: "PerformLive.exe --scan-plugins" scans the standard VST3
        // folders through the separate scanner process, prints what it found,
        // saves the list for the app, and quits. Proves the subprocess path
        // without a window, and seeds the Instrument menu on a new machine.
        if (commandLine.contains ("--scan-plugins"))
        {
            struct CliScan final : public juce::Thread
            {
                CliScan() : juce::Thread ("plugin scan") {}
                void run() override
                {
                    ezinst::PluginLibrary lib;
                    const auto pedal = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("performlive-scan-pedal.txt");
                    int files = 0;
                    for (auto* format : lib.formatManager().getFormats())
                    {
                        juce::PluginDirectoryScanner scanner (lib.list(), *format, format->getDefaultLocationsToSearch(), true, pedal, true);
                        juce::String name;
                        while (! threadShouldExit())
                        {
                            const auto next = scanner.getNextPluginFileThatWillBeScanned();
                            if (! scanner.scanNextFile (true, name)) break;
                            ++files;
                            std::printf ("  scanned %s\n", next.toRawUTF8());
                            std::fflush (stdout);
                        }
                    }
                    std::printf ("\n%d files scanned. Instruments:\n", files);
                    for (const auto& d : lib.instruments())
                        std::printf ("  [%s] %s -- %s\n", d.pluginFormatName.toRawUTF8(), d.name.toRawUTF8(), d.manufacturerName.toRawUTF8());
                    std::printf ("%d instruments, %d plugins in total\n", lib.instruments().size(), lib.list().getNumTypes());
                    std::fflush (stdout);
                    if (auto* settings = SessionComponent::getAppSettings()) lib.saveTo (*settings);
                    juce::MessageManager::callAsync ([] { juce::JUCEApplication::getInstance()->quit(); });
                }
            };
            cliScan = std::make_unique<CliScan>();
            cliScan->startThread();
            return;
        }

        if (commandLine.contains ("--selftest-persistence"))
        {
            std::printf ("Milestone 12 persistence self-test\n");
            const bool ok = runPersistenceSelfTest();
            std::printf ("\n%s\n", ok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--selftest-library"))
        {
            std::printf ("Milestone 16 library self-test\n");
            const bool ok = runLibrarySelfTest();
            std::printf ("\n%s\n", ok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--selftest-beta"))
        {
            std::printf ("Beta end-date self-test (compile-date parsing, end date, fail-closed)\n");
            const bool ok = performbeta::runSelfTest();
            std::printf ("\n%s\n", ok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        // Owner: "the app doesn't see my UMC driver." Lists every audio
        // driver type this build knows about and every device each one can
        // see, exactly as Settings > Audio will offer them -- so "is ASIO
        // compiled in, and does it find the interface?" is a one-line answer
        // rather than a screenshot. Also handy in a tester's bug report.
        if (commandLine.contains ("--list-audio-devices"))
        {
            juce::AudioDeviceManager dm;   // getAvailableDeviceTypes() creates them lazily
            std::printf ("Audio driver types in this build:\n");
            for (auto* type : dm.getAvailableDeviceTypes())
            {
                type->scanForDevices();
                std::printf ("\n  [%s]\n", type->getTypeName().toRawUTF8());
                auto names = type->getDeviceNames (false);
                if (names.isEmpty()) std::printf ("    (no output devices found)\n");
                for (auto& n : names)
                {
                    std::unique_ptr<juce::AudioIODevice> dev (type->createDevice (n, {}));
                    const int outs = dev != nullptr ? dev->getOutputChannelNames().size() : -1;
                    std::printf ("    %-50s %d outputs\n", n.toRawUTF8(), outs);
                }
            }
            std::printf ("\n");
            quit();
            return;
        }

        if (commandLine.contains ("--selftest-importer"))
        {
            std::printf ("Milestone 16 importer self-test\n");
            const bool ok = runImporterSelfTest();
            std::printf ("\n%s\n", ok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        // roadmap "Sprint 5: reliability" -- Logging. Until this, EzPlay had
        // no log file at all, making a crash on a user's machine
        // undiagnosable after the fact. Set up AFTER the --selftest-*
        // early-returns above so those stay lightweight and don't spam a
        // log file for what's effectively a CI-style check, not a real
        // session. JUCE's own rotating-file logger self-manages retention
        // and writes to the platform's standard app-data/Logs location;
        // setCurrentLogger() makes juce::Logger::writeToLog() callable
        // anywhere in the app from here on.
        // Logs are machine state: %LOCALAPPDATA%/Amanorsac Studio/PerformLive/Logs.
        {
            auto logDir = productpaths::machineState().getChildFile ("Logs");
            logDir.createDirectory();
            fileLogger = std::make_unique<juce::FileLogger> (logDir.getChildFile ("PerformLive.log"), "PerformLive session log");
        }
        juce::Logger::setCurrentLogger (fileLogger.get());
        juce::Logger::writeToLog ("PerformLive " + getApplicationVersion() + " starting up");
        juce::SystemStats::setApplicationCrashHandler (performliveCrashHandler);

        // PerformLive font stack -- installed BEFORE the window exists so
        // the very first paint already uses Inter/JetBrains Mono. See
        // PerformLookAndFeel's own comment for why one default LnF retypes
        // the whole app.
        performLookAndFeel = std::make_unique<PerformLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (performLookAndFeel.get());

        // The beta ends on a fixed date (Beta.h). From then on the app opens only
        // a notice saying so -- never the performance window, and never an audio
        // engine half-started behind it. --simulate-beta-ended shows the same
        // notice without touching the system clock, so it can be checked.
        const auto betaEndsAt = performbeta::expiry();
        if (commandLine.contains ("--simulate-beta-ended")
             || performbeta::hasEnded (juce::Time::getCurrentTime(), betaEndsAt))
        {
            juce::Logger::writeToLog ("Beta ended " + performbeta::dateText (betaEndsAt) + " -- showing the end-of-beta notice");
            expiredWindow = std::make_unique<creators::BetaEndedWindow> (betaEndsAt);
            return;
        }

        mainWindow.reset (new MainWindow (performbeta::kLabel));

        // A report from a previous run: say so once, keep the file under a
        // dated name so it is not reported again, and offer to show it.
        if (const auto pending = pendingCrashReportFile(); pending.existsAsFile())
        {
            const auto kept = crashReportFolder().getChildFile ("PerformLive-crash-"
                                  + juce::Time::getCurrentTime().formatted ("%Y-%m-%d-%H%M%S") + ".txt");
            pending.moveFileTo (kept);
            juce::Logger::writeToLog ("Previous run crashed; report kept at " + kept.getFullPathName());
            juce::MessageManager::callAsync ([kept]
            {
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon,
                    "PerformLive closed unexpectedly last time",
                    "A report was saved so the cause can be fixed:\n" + kept.getFullPathName()
                    + "\n\nPlease email it to amanorsac@gmail.com. Your project and library are untouched.",
                    "Show the file", "OK", nullptr,
                    juce::ModalCallbackFunction::create ([kept] (int r) { if (r == 1) kept.revealToUser(); }));
            });
        }
    }
    void shutdown() override
    {
        if (cliScan != nullptr) cliScan->stopThread (5000);
        juce::Logger::writeToLog ("PerformLive shutting down cleanly");
        mainWindow = nullptr;
        expiredWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        performLookAndFeel.reset();
        juce::Logger::setCurrentLogger (nullptr);
        fileLogger.reset();
    }
    void systemRequestedQuit() override
    {
        // the close button, Alt+F4 and Windows shutting down all come here
        if (mainWindow != nullptr)
            if (auto* session = dynamic_cast<SessionComponent*> (mainWindow->getContentComponent()))
            {
                session->requestQuit ([] { juce::JUCEApplication::getInstance()->quit(); });
                return;
            }
        quit();
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<creators::BetaEndedWindow> expiredWindow;   // shown instead of mainWindow once the beta has ended
    std::unique_ptr<PerformLookAndFeel> performLookAndFeel;   // app-wide font stack -- see its class comment
    std::unique_ptr<juce::FileLogger> fileLogger;
    std::unique_ptr<ezinst::ScannerSubprocess> scannerSubprocess;   // PX-D: only in the scanner worker process
    std::unique_ptr<juce::Thread> cliScan;                            // PX-D: --scan-plugins
};

START_JUCE_APPLICATION (EzPlayApplication)
