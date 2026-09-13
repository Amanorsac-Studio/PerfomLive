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
//  Editing sections happens here too, on the timeline's ruler: double-click
//  adds a section at that bar, drag a boundary to move it, right-click a
//  section for its name, colour and flags. "Auto-section" drafts a typical
//  worship structure to be corrected rather than built from nothing.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "Arrangement.h"
#include "Deck.h"        // ezdeck::kNumLayers -- the lane count follows the engine
#include "UiArt.h"

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
//  The timeline: ruler + section blocks + four lanes + playhead.
//==============================================================================
class Timeline : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Timeline (PlaybackHost& h) : host (h) { setWantsKeyboardFocus (false); }

    std::function<void (int sectionIndex, juce::Point<int> screenPos)> onSectionMenu;
    std::function<void (int bar)> onAddSectionAt;

    void rebuildPeaks()
    {
        const int song = host.currentSongIndex();
        for (int l = 0; l < ezdeck::kNumLayers; ++l)
        {
            auto& p = peaks[(size_t) l];
            p.assign (kPeakRes, 0.0f);
            const auto* samples = song >= 0 ? host.layerSamples (song, l) : nullptr;
            hasAudio[(size_t) l] = samples != nullptr && ! samples->empty();
            if (! hasAudio[(size_t) l]) continue;
            const size_t n = samples->size();
            for (int b = 0; b < kPeakRes; ++b)
            {
                const size_t s0 = (size_t) ((double) b / kPeakRes * (double) n);
                const size_t s1 = (size_t) ((double) (b + 1) / kPeakRes * (double) n);
                float mx = 0.0f;
                for (size_t i = s0; i < s1 && i < n; ++i) mx = (std::max) (mx, std::fabs ((*samples)[i]));
                p[(size_t) b] = mx;
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
        const int bars = song >= 0 ? (std::max) (1, host.songLengthBars (song)) : 0;
        if (arr == nullptr || bars <= 0)
        {
            g.setColour (tokens::faint);
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText ("Add a song to the setlist to see its timeline", r, juce::Justification::centred);
            return;
        }

        const auto inner = r.reduced (12, 10);
        const float pxPerBar = (float) inner.getWidth() / (float) bars;
        const auto ruler = inner.withHeight (kRulerHeight);
        const auto lanesArea = inner.withTrimmedTop (kRulerHeight + 6);
        const int laneCount = (std::max) (1, (int) visibleLanes.size());
        const int laneH = lanesArea.getHeight() / laneCount;

        // ---- section blocks on the ruler ----
        const int current = host.currentSection();
        const int queued  = host.queuedSection();
        for (int i = 0; i < (int) arr->sections.size(); ++i)
        {
            const int b0 = arr->sectionStartBar (i), b1 = arr->sectionEndBar (i);
            if (b1 <= b0) continue;
            auto block = juce::Rectangle<float> (inner.getX() + b0 * pxPerBar, (float) ruler.getY(),
                                                 (b1 - b0) * pxPerBar, (float) ruler.getHeight()).reduced (1.0f, 0.0f);
            auto col = sectionColour (*arr, i);
            const auto& s = arr->sections[(size_t) i];
            const bool dimmed = s.skip || s.optional;
            g.setColour (col.withAlpha (i == current ? 0.95f : (dimmed ? 0.18f : 0.45f)));
            g.fillRoundedRectangle (block, 5.0f);
            if (i == queued)
            {
                g.setColour (tokens::queued);
                g.drawRoundedRectangle (block, 5.0f, 2.0f);
            }
            g.setColour (i == current ? juce::Colours::black.withAlpha (0.85f) : tokens::bright);
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            juce::String label = juce::String (s.name);
            if (s.loopOnEntry) label += " \xe2\x9f\xb3";
            if (s.pauseAfter)  label += " \xe2\x8f\xb8";
            if (s.optional)    label += " (opt)";
            g.drawFittedText (juce::String (juce::CharPointer_UTF8 (label.toRawUTF8())),
                              block.reduced (6.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        }

        // ---- bar grid across the lanes ----
        g.setColour (tokens::border.withAlpha (0.5f));
        const int every = pxPerBar >= 28.0f ? 1 : pxPerBar >= 12.0f ? 2 : 4;
        for (int b = 0; b <= bars; b += every)
        {
            const float x = inner.getX() + b * pxPerBar;
            g.drawVerticalLine ((int) x, (float) lanesArea.getY(), (float) lanesArea.getBottom());
            if (pxPerBar >= 18.0f)
            {
                g.setColour (tokens::faint);
                g.setFont (juce::Font (juce::FontOptions (9.0f)));
                g.drawText (juce::String (b + 1), (int) x + 3, ruler.getBottom() - 2, 30, 10, juce::Justification::left);
                g.setColour (tokens::border.withAlpha (0.5f));
            }
        }

        // ---- lanes ----
        // Same eight hues as the deck columns, so a lane and its column
        // are recognisably the same layer.
        static const juce::Colour laneCols[ezdeck::kNumLayers] = {
            juce::Colour (0xff00d9ffu), juce::Colour (0xffa855f7u),
            juce::Colour (0xffff2d95u), juce::Colour (0xffffa62bu),
            juce::Colour (0xff3dffc0u), juce::Colour (0xff4d7cffu),
            juce::Colour (0xffb6ff2eu), juce::Colour (0xffff5c3bu) };
        for (int slotIdx = 0; slotIdx < (int) visibleLanes.size(); ++slotIdx)
        {
            const int l = visibleLanes[(size_t) slotIdx];
            auto lane = juce::Rectangle<int> (lanesArea.getX(), lanesArea.getY() + slotIdx * laneH, lanesArea.getWidth(), laneH).reduced (0, 3);
            const bool on = host.layerEnabled (song, l);
            g.setColour (tokens::card.withAlpha (0.5f));
            g.fillRoundedRectangle (lane.toFloat(), 4.0f);

            if (hasAudio[(size_t) l])
            {
                const float mid = (float) lane.getCentreY();
                const float half = (float) lane.getHeight() * 0.46f;
                g.setColour (laneCols[l].withAlpha (on ? 0.9f : 0.22f));
                const int w = lane.getWidth();
                for (int x = 0; x < w; ++x)
                {
                    const int b = juce::jlimit (0, kPeakRes - 1, (int) ((double) x / w * kPeakRes));
                    const float h = peaks[(size_t) l][(size_t) b] * half;
                    if (h > 0.5f) g.drawVerticalLine (lane.getX() + x, mid - h, mid + h);
                }
            }
            g.setColour (on ? tokens::bright : tokens::faint);
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (host.layerName (song, l), lane.reduced (6, 0), juce::Justification::topLeft, true);
        }

        // ---- loop shading ----
        if (host.isLoopingSection() && current >= 0)
        {
            const int b0 = arr->sectionStartBar (current), b1 = arr->sectionEndBar (current);
            g.setColour (tokens::queued.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float> (inner.getX() + b0 * pxPerBar, (float) lanesArea.getY(), (b1 - b0) * pxPerBar, (float) lanesArea.getHeight()));
        }

        // ---- playhead ----
        const double bar = host.currentBar();
        const float px = inner.getX() + (float) bar * pxPerBar;
        g.setColour (tokens::play);
        g.fillRect (juce::Rectangle<float> (px - 1.0f, (float) ruler.getY(), 2.0f, (float) (lanesArea.getBottom() - ruler.getY())));
        juce::Path tri;
        tri.addTriangle (px - 6.0f, (float) ruler.getY() - 1.0f, px + 6.0f, (float) ruler.getY() - 1.0f, px, (float) ruler.getY() + 6.0f);
        g.fillPath (tri);

        // ---- drag feedback ----
        if (draggingBoundary >= 0)
        {
            g.setColour (tokens::bright.withAlpha (0.8f));
            const float x = inner.getX() + (float) dragBar * pxPerBar;
            g.drawVerticalLine ((int) x, (float) ruler.getY(), (float) lanesArea.getBottom());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr) return;
        const auto geo = geometry();
        if (geo.bars <= 0) return;

        const int bar = barAt (e.x, geo);

        if (e.mods.isPopupMenu())
        {
            const int s = arr->sectionAtBar (bar);
            if (s >= 0 && onSectionMenu) onSectionMenu (s, e.getScreenPosition());
            return;
        }

        if (e.y < geo.ruler.getBottom())
        {
            // near a boundary? grab it for dragging
            for (int i = 1; i < (int) arr->sections.size(); ++i)
            {
                const float x = geo.inner.getX() + arr->sectionStartBar (i) * geo.pxPerBar;
                if (std::abs ((float) e.x - x) <= 6.0f) { draggingBoundary = i; dragBar = arr->sectionStartBar (i); return; }
            }
            // otherwise a click on a block queues a jump to it
            const int s = arr->sectionAtBar (bar);
            if (s >= 0) host.jumpToSection (s);
            return;
        }

        // lanes: click to seek (quantised by the host), click the name to mute
        const int song = host.currentSongIndex();
        const int lane = juce::jlimit (0, 3, (e.y - geo.lanes.getY()) / (std::max) (1, geo.lanes.getHeight() / 4));
        if (e.x < geo.inner.getX() + 70 && song >= 0) host.toggleLayer (song, lane);
        else host.seekToBar ((double) bar);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingBoundary < 0) return;
        const auto geo = geometry();
        dragBar = barAt (e.x, geo);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (draggingBoundary < 0) return;
        if (auto* arr = host.currentArrangement())
            if (arr->moveSection (draggingBoundary, dragBar))
                host.arrangementEdited();
        draggingBoundary = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto geo = geometry();
        if (geo.bars <= 0 || e.y >= geo.ruler.getBottom()) return;
        if (onAddSectionAt) onAddSectionAt (barAt (e.x, geo));
    }

private:
    static constexpr int kRulerHeight = 26;
    static constexpr int kPeakRes = 1024;

    struct Geo { juce::Rectangle<int> inner, ruler, lanes; int bars { 0 }; float pxPerBar { 0 }; };

    Geo geometry() const
    {
        Geo g;
        const int song = host.currentSongIndex();
        g.bars = song >= 0 ? (std::max) (1, host.songLengthBars (song)) : 0;
        g.inner = getLocalBounds().reduced (12, 10);
        g.ruler = g.inner.withHeight (kRulerHeight);
        g.lanes = g.inner.withTrimmedTop (kRulerHeight + 6);
        g.pxPerBar = g.bars > 0 ? (float) g.inner.getWidth() / (float) g.bars : 0.0f;
        return g;
    }

    int barAt (int x, const Geo& g) const
    {
        if (g.pxPerBar <= 0.0f) return 0;
        return juce::jlimit (0, (std::max) (0, g.bars - 1), (int) std::floor ((float) (x - g.inner.getX()) / g.pxPerBar));
    }

    PlaybackHost& host;
    std::array<std::vector<float>, ezdeck::kNumLayers> peaks;
    std::array<bool, ezdeck::kNumLayers> hasAudio {};
    // PX-B: the lanes actually drawn, in order. A song with four stems
    // still looks like a four-stem song -- an empty layer is not given a
    // lane, because a row of blank lanes on a music stand is noise.
    std::vector<int> visibleLanes;
    int draggingBoundary { -1 };
    int dragBar { 0 };
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
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Rename...");
            m.addItem (2, "Remove from setlist");
            m.showMenuAsync (juce::PopupMenu::Options(), [this, row] (int r)
            {
                if (r == 1) host.renameSong (row);
                if (r == 2) host.removeSong (row);
            });
            return;
        }
        host.selectSong (row);
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
        timeline.onAddSectionAt = [this] (int bar) { addSectionAt (bar); };

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
            g.drawFittedText (juce::String (juce::CharPointer_UTF8 (name.c_str())), inner, juce::Justification::centredLeft, 1);
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
        autoSectionButton.setBounds (e.removeFromLeft (110)); e.removeFromLeft (8);
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

    void addSectionAt (int bar)
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr) return;
        auto* w = new juce::AlertWindow ("New section", "Starts at bar " + juce::String (bar + 1), juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", "Section " + juce::String (arr->sections.size() + 1), "Name");
        w->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, bar, w] (int r)
        {
            std::unique_ptr<juce::AlertWindow> owner (w);
            if (r != 1) return;
            if (auto* a = host.currentArrangement())
            {
                ezarr::Section s;
                s.name = w->getTextEditorContents ("name").trim().substring (0, 40).toStdString();
                s.startBar = bar;
                a->addSection (s);
                host.arrangementEdited();
                songChanged();
            }
        }), false);
    }

    void autoSection()
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr || arr->lengthBars <= 0) return;
        juce::PopupMenu m;
        m.addSectionHeader ("Draft sections every...");
        for (int n : { 4, 8, 16 }) m.addItem (n, juce::String (n) + " bars");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (autoSectionButton), [this] (int n)
        {
            if (n <= 0) return;
            if (auto* a = host.currentArrangement())
            {
                a->autoSection (n, ezarr::worshipSectionNames());
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
        m.addSeparator();
        m.addItem (9, "Delete section", s > 0);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                         [this, s] (int r)
        {
            auto* a = host.currentArrangement();
            if (r == 0 || a == nullptr || s >= (int) a->sections.size()) return;
            auto& sec = a->sections[(size_t) s];
            switch (r)
            {
                case 1: host.jumpToSection (s); return;
                case 2: renameSection (s); return;
                case 3: sec.loopOnEntry = ! sec.loopOnEntry; break;
                case 4: sec.pauseAfter  = ! sec.pauseAfter;  break;
                case 5: sec.skip        = ! sec.skip;        break;
                case 6: sec.optional    = ! sec.optional;    break;
                case 9: a->removeSection (s); break;
                default: if (r >= 100 && r < 108) sec.colourArgb = ezarr::defaultSectionColour (r - 100); break;
            }
            host.arrangementEdited();
            songChanged();
        });
    }

    void renameSection (int s)
    {
        auto* arr = host.currentArrangement();
        if (arr == nullptr) return;
        auto* w = new juce::AlertWindow ("Rename section", "", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", juce::String (juce::CharPointer_UTF8 (arr->sections[(size_t) s].name.c_str())), "Name");
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, s, w] (int r)
        {
            std::unique_ptr<juce::AlertWindow> owner (w);
            if (r != 1) return;
            if (auto* a = host.currentArrangement())
                if (s < (int) a->sections.size())
                {
                    a->sections[(size_t) s].name = w->getTextEditorContents ("name").trim().substring (0, 40).toStdString();
                    host.arrangementEdited();
                    songChanged();
                }
        }), false);
    }

    PlaybackHost& host;
    Timeline timeline;
    SetlistPanel setlist;
    juce::Rectangle<int> headerArea, dotsArea;
    juce::TextButton prevSongButton, prevSectionButton, playButton, countInButton, nextSectionButton, nextSongButton,
                     loopButton, jumpNowButton, cancelButton, autoSectionButton;
    juce::ComboBox jumpModeBox, countInBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackView)
};

} // namespace ezplayback
