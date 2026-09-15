#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <vector>

//==============================================================================
//  DeckCard — one tab in the deck grid.
//
//  Self-contained: depends only on JUCE, never on ezdeck::Layer or any engine
//  type, so it can be unit-tested, reused, and moved without dragging the
//  engine along. The owner pushes state in; the card pushes gestures out.
//
//  PERFORMANCE — the reason this class exists rather than a raw paint routine:
//  a 4x8 grid is 32 waveforms. Recomputing min/max from raw samples on every
//  repaint would be 32 passes over full audio buffers per frame. setAudio()
//  builds a fixed-resolution peak cache ONCE at load; paint() only reads it.
//
//  TOUCH — every context menu in this app is currently right-click only, which
//  does not exist on iPad. This card implements long-press (650 ms) as a first-
//  class path to the same menu, matching SceneButton's existing hold pattern so
//  the gesture is consistent app-wide. (The "..." glyph that used to be a
//  third path is gone -- owner: not touch friendly.)
//==============================================================================
class DeckCard : public juce::Component,
                 public juce::DragAndDropTarget,
                 public juce::FileDragAndDropTarget,   // OS file drops -- see onFilesDropped
                 private juce::Timer
{
public:
    //== state pushed in by the owner =========================================
    // SPEC_PERFORM_V2 GROUP A: the three-state model is now literally
    // (deselected/selected-stopped/playing), mapped onto the existing
    // names -- `disabled` = deselected (greyed; every non-selected deck
    // looks like this now, regardless of any layer's own mute state),
    // `armed` = selected + stopped (full colour, outlined, static),
    // `live` = playing (brighter, beat-synced pulse). `empty` is the
    // separate "nothing loaded here" case, unaffected by selection.
    enum class State { empty, disabled, armed, live };

    //== gestures pushed out ==================================================
    std::function<void()> onTap;          // arm/disarm, or open loader when empty
    std::function<void()> onDoubleTap;    // open clip editor
    std::function<void()> onMenu;         // long-press or right-click

    // Phase 1.1 P1 "Deck Loading": fired when a Library sample is dropped
    // here -- the string is whatever the drag source put in its description
    // (Main.cpp's LibrarySampleCard uses its own assetId), stripped of the
    // "ezplay-asset:" prefix isInterestedInDragSource() below requires. This
    // card only recognizes that one drag-source vocabulary -- an unrelated
    // drag (e.g. an OS file drag) is simply not interested and JUCE routes
    // it elsewhere; no engine-level asset-type check happens here at all,
    // since DeckCard is deliberately engine-agnostic (see this file's own
    // header comment) -- the owner decides what to do with the id.
    std::function<void (const juce::String&)> onAssetDropped;

    // Owner request ("the whole app should accept drag and drop from
    // Windows"): OS file drops -- same owner-decides philosophy as
    // onAssetDropped above. Unwired (nullptr) = this card refuses file
    // drags entirely, so JUCE routes them to an ancestor target instead.
    std::function<void (const juce::StringArray&)> onFilesDropped;

    DeckCard() { setWantsKeyboardFocus (false); }

    //==========================================================================
    //  State
    //==========================================================================
    void setAccent (juce::Colour c)               { accent = c; repaint(); }
    void setClipName (const juce::String& n)      { clipName = n; repaint(); }
    void setBpm (double b)                        { bpm = b; repaint(); }

    void setState (State s)
    {
        if (state == s) return;
        state = s;
        if (s != State::live) pulsePhase = 0.0;
        repaint();
    }

    State getState() const noexcept { return state; }

    // Bug report: "clicking a layer tab should light up when unmuted and
    // grey out when muted -- for now it stays the same." GROUP A's
    // three-state model above (empty/disabled/armed/live) is selection-only
    // by design and deliberately dropped its own separate mute dimming; this
    // restores a mute indication WITHOUT touching that model, by fading the
    // whole card the same way MixerChannelStrip::refreshDim() already dims a
    // muted mixer channel (setAlpha), rather than inventing a second dimming
    // scheme. Independent of `state`: a muted-but-selected (armed/live) card
    // still shows its full-colour selection look, just faded.
    void setMuted (bool m) { setAlpha (m ? 0.45f : 1.0f); }

    /** Phase within the current beat, 0..1, for the live pulse.
        Driven by the owner's existing UI timer from the deck's ACTUAL playback
        position — not a free-running animation — so a card that becomes visible
        mid-bar is already in step rather than starting its own cycle. */
    void setBeatPhase (double phase01)
    {
        if (state != State::live) return;
        pulsePhase = juce::jlimit (0.0, 1.0, phase01);
        repaint();
    }

    /** Build the peak cache. Call once when a clip loads, not per frame.
        Pass the mono or left-channel samples; kPeakResolution buckets are
        stored regardless of source length, so paint() cost is constant. */
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

    void clearAudio()
    {
        hasAudio = false;
        peaksMin.clear();
        peaksMax.clear();
        repaint();
    }

    //==========================================================================
    //  Painting
    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        // The halo below is drawn OUTSIDE these bounds, so the card must sit
        // inset from its own component edge or every ring is clipped away --
        // which is precisely why the old 0.5px inset made a "glowing" live
        // card look identical to a flat one.
        const auto bounds = getLocalBounds().toFloat().reduced (5.0f);
        const float radius = 8.0f;

        // Visual-polish pass (LoopLab reference): same layered-alpha
        // fake-glow SceneButton/SignatureRailButton/DeckTriggerCell already
        // use for their own "this is live right now" state -- a card that's
        // actually playing gets a soft glow behind it, not just a flat wash.
        if (state == State::live)
        {
            const float halo = livePulseBrightness();
            for (int i = 4; i >= 1; --i)
            {
                const float expand = (float) i * 1.3f;
                g.setColour (accent.withAlpha (0.20f * halo / (float) i));
                g.fillRoundedRectangle (bounds.expanded (expand), radius + expand);
            }
        }

        // ---- fill ----------------------------------------------------------
        g.setColour (fillColour());
        g.fillRoundedRectangle (bounds, radius);

        // a live card gets a faint wash of its own accent, so "playing" reads
        // from across a room and not just from the border
        if (state == State::live)
        {
            juce::ColourGradient wash (accent.withAlpha (0.22f), bounds.getCentreX(), bounds.getBottom(),
                                       accent.withAlpha (0.03f), bounds.getCentreX(), bounds.getY(), false);
            g.setGradientFill (wash);
            g.fillRoundedRectangle (bounds, radius);
        }

        // ---- border --------------------------------------------------------
        if (state == State::empty)
        {
            juce::Path outline, dashed;
            outline.addRoundedRectangle (bounds, radius);
            const float dashes[] { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (juce::Colour (0xff2b2b4d));
            g.strokePath (dashed, juce::PathStrokeType (1.0f));
        }
        else
        {
            // SPEC_PERFORM_V2 GROUP A: `armed` (selected + stopped) gets the
            // same 2px "outlined" thickness as `live` -- the spec's own
            // language for that state is "full colour, outlined," which
            // needs to read as distinctly more present than the thin 1px
            // deselected border, even though (unlike `live`) it has no wash
            // fill and no pulse.
            g.setColour (borderColour());
            g.drawRoundedRectangle (bounds, radius, (state == State::live || state == State::armed) ? 2.0f : 1.0f);
        }

        // ---- accent light-up (owner direction, replacing the earlier
        // rainbow ring: "make the colours already on the deck light up --
        // the colour lines around -- not completely new colours"): layered
        // concentric strokes of this card's OWN accent colour just inside
        // the border, so selecting a deck makes its colour visibly glow.
        // Drawn INSIDE the bounds -- an outward glow would be clipped away
        // by this component's own paint area (the exact bug the old ring
        // had). `armed` glows steadily; `live` breathes with the beat via
        // the same livePulseBrightness() curve the border pulse already
        // uses, so it reads as one animation, not two.
        if (state == State::armed || state == State::live)
        {
            const float pulse = (state == State::live) ? livePulseBrightness() : 1.0f;
            for (int i = 3; i >= 1; --i)
            {
                const float inset = (float) i * 1.6f;
                g.setColour (accent.withAlpha (0.30f * pulse / (float) i));
                g.drawRoundedRectangle (bounds.reduced (inset), juce::jmax (2.0f, radius - inset * 0.5f), 2.5f);
            }
        }

        // ---- moving beat indicator (Phase 1.1 P1 "Deck Visual Feedback") ---
        // A small marker sweeping left-to-right across the top edge, one
        // sweep per beat -- driven by the SAME pulsePhase setBeatPhase()
        // already sets from the deck's REAL playback position (see that
        // method's own comment), not a free-running animation of its own.
        // Only drawn in the live state, matching setBeatPhase()'s own
        // "state != live -> pulsePhase reset to 0, never updated again"
        // guard above -- a stopped-but-still-selected deck simply freezes
        // the marker wherever the last beat left it, rather than resetting
        // or implying motion that isn't happening.
        if (state == State::live)
        {
            const float margin = 5.0f;
            const float travel  = juce::jmax (0.0f, bounds.getWidth() - margin * 2.0f);
            const float x = bounds.getX() + margin + travel * (float) pulsePhase;
            g.setColour (accent);
            g.fillRoundedRectangle (juce::Rectangle<float> (x - 2.0f, bounds.getY() + 1.5f, 4.0f, 3.0f), 1.5f);
        }

        // ---- drag-hover highlight (Phase 1.1 P1 "Deck Loading") ------------
        // Drawn regardless of state -- an empty card can be dropped into
        // just as validly as a loaded one (that's a Replace).
        if (dragHover)
        {
            g.setColour (accent.withAlpha (0.6f));
            g.drawRoundedRectangle (bounds, radius, 2.5f);
        }

        // ---- empty: just a centred plus ------------------------------------
        if (state == State::empty)
        {
            if (clipName.isNotEmpty())
            {
                // PX-C: an empty slot in a live-input column says what it is
                // carrying rather than inviting a file.
                const auto label = clipName.upToFirstOccurrenceOf ("  ", false, false);
                const auto detail = clipName.fromFirstOccurrenceOf ("  ", false, false).trim();
                auto box = getLocalBounds().withSizeKeepingCentre (juce::jmin (getWidth() - 20, 230), 40);
                g.setColour (accent.withAlpha (0.16f));
                g.fillRoundedRectangle (box.toFloat(), 8.0f);
                g.setColour (accent.withAlpha (0.8f));
                g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 8.0f, 1.0f);
                g.setColour (accent.brighter (0.2f));
                g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)).withExtraKerningFactor (0.08f));
                g.drawText (label, box.removeFromTop (22), juce::Justification::centred, false);
                g.setColour (juce::Colour (0xffa3a6cc));
                g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::plain)));
                g.drawText (detail, box, juce::Justification::centred, false);
                return;
            }
            g.setColour (juce::Colour (0xff6f7099));
            g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::plain)));
            g.drawText ("+", getLocalBounds(), juce::Justification::centred, false);
            return;
        }

        auto area = bounds.toNearestInt().reduced (9, 7);

        // ---- name + bpm ----------------------------------------------------
        auto textArea = area.removeFromTop (kTextBlockHeight);

        g.setColour (state == State::disabled ? juce::Colour (0xffa3a6cc)
                                              : juce::Colour (0xfff2f0ff));
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (clipName, textArea.removeFromTop (17),
                    juce::Justification::centredLeft, true);

        g.setColour (juce::Colour (0xff6f7099));
        // Visual-polish pass: monospace for the numeric readout, matching
        // BpmReadout's own convention (see its own comment).
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::plain)));
        g.drawText (bpm > 0.0 ? juce::String (bpm, 1) + " BPM" : juce::String ("—"),
                    textArea, juce::Justification::centredLeft, false);

        // (The "..." menu glyph is gone -- owner: not touch friendly; the
        // menu stays reachable via right-click and long-press, both below.)

        // ---- waveform ------------------------------------------------------
        drawWaveform (g, area.reduced (0, 2).toFloat());
    }

    void resized() override {}

    //==========================================================================
    //  Interaction
    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        // right-click: straight to the menu (mouse path, retained)
        if (e.mods.isPopupMenu())
        {
            if (onMenu != nullptr) onMenu();
            return;
        }

        held = false;
        startTimer (kHoldMs);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        stopTimer();

        // a completed long-press already fired onMenu; do not also fire onTap
        if (held || e.mods.isPopupMenu()) return;

        // ignore a drag that wandered off the card
        if (! getLocalBounds().contains (e.getPosition())) return;

        // Owner #10 ("introduce a small delay so double-clicking to open
        // the edit window does not pause the tabs"): the single-tap is
        // deferred just under the double-click window; a double-click
        // bumps the serial first, so the deferred tap sees a stale serial
        // and does nothing. SafePointer guards against the card being
        // destroyed before the delay fires.
        ++pendingTapSerial;
        const int serial = pendingTapSerial;
        juce::Component::SafePointer<DeckCard> safe (this);
        juce::Timer::callAfterDelay (220, [safe, serial]
        {
            if (safe != nullptr && safe->pendingTapSerial == serial && safe->onTap != nullptr)
                safe->onTap();
        });
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        stopTimer();
        held = false;
        ++pendingTapSerial;   // cancel the deferred single-tap (owner #10)
        if (onDoubleTap != nullptr) onDoubleTap();
    }

    // Owner #10 -- public so the deferred-tap lambda (a non-member) can
    // check it through the SafePointer; see mouseUp() above.
    int pendingTapSerial { 0 };

    //==========================================================================
    //  juce::DragAndDropTarget
    //==========================================================================
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return details.description.toString().startsWith ("ezplay-asset:");
    }

    void itemDragEnter (const SourceDetails&) override { dragHover = true;  repaint(); }
    void itemDragExit  (const SourceDetails&) override { dragHover = false; repaint(); }

    void itemDropped (const SourceDetails& details) override
    {
        dragHover = false;
        if (onAssetDropped != nullptr)
            onAssetDropped (details.description.toString().fromFirstOccurrenceOf ("ezplay-asset:", false, false));
        repaint();
    }

    //==========================================================================
    //  juce::FileDragAndDropTarget -- OS files (owner: "the whole app should
    //  accept drag and drop from Windows"). The owner (Main.cpp) decides
    //  what to do with the paths -- this card stays engine-agnostic, exactly
    //  like onAssetDropped above. Audio-extension filtering happens in the
    //  owner's isInterestedInFileDrag gate via onFilesDropped being wired.
    //==========================================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        if (onFilesDropped == nullptr) return false;
        for (const auto& f : files)
            for (auto* ext : { "*.wav", "*.aif", "*.aiff", "*.flac", "*.mp3", "*.ogg" })
                if (f.matchesWildcard (ext, true)) return true;
        return false;
    }
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true;  repaint(); }
    void fileDragExit  (const juce::StringArray&) override           { dragHover = false; repaint(); }
    void filesDropped  (const juce::StringArray& files, int, int) override
    {
        dragHover = false;
        if (onFilesDropped != nullptr) onFilesDropped (files);
        repaint();
    }

private:
    //==========================================================================
    void timerCallback() override
    {
        stopTimer();
        held = true;
        if (onMenu != nullptr) onMenu();
    }

    juce::Colour fillColour() const
    {
        if (state == State::empty) return juce::Colour (0xff0f0f1d);
        return juce::Colour (0xff151527);
    }

    juce::Colour borderColour() const
    {
        switch (state)
        {
            case State::live:     return accent.withBrightness (livePulseBrightness());
            // SPEC_PERFORM_V2 GROUP A: `armed` = selected + stopped --
            // "full colour, outlined," so the border is the accent at full
            // strength (was a dimmed 0.70 alpha under the old "not the
            // active deck but not muted either" meaning this state used to
            // have -- see the state-assignment comment in timerCallback()
            // for the full before/after).
            case State::armed:    return accent;
            case State::disabled: return juce::Colour (0xff2b2b4d);
            case State::empty:
            default:              return juce::Colour (0xff2b2b4d);
        }
    }

    /** Eases 1.0 -> 0.65 across each beat. Not a blink: a blink is noise on a
        dark stage, an ease reads as a pulse in time. */
    float livePulseBrightness() const
    {
        const float t = (float) pulsePhase;
        const float eased = 1.0f - (t * t);          // fast attack, slow decay
        return juce::jlimit (0.55f, 1.0f, 0.65f + 0.35f * eased);
    }

    void drawWaveform (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        if (! hasAudio || peaksMax.empty() || r.getHeight() < 4.0f) return;

        const float alpha = state == State::live     ? 1.00f
                          : state == State::armed    ? 0.85f
                                                     : 0.30f;

        // Brightest along the centre line and falling off towards the peaks,
        // so a loud passage reads as a lit band rather than a solid block.
        // Set once, before the loop: a gradient fill is state on the
        // Graphics context, and the per-x drawLine calls inherit it.
        juce::ColourGradient wf (accent.withAlpha (alpha * 0.28f), r.getX(), r.getY(),
                                 accent.withAlpha (alpha * 0.28f), r.getX(), r.getBottom(), false);
        wf.addColour (0.5, accent.withAlpha (alpha));
        g.setGradientFill (wf);

        const float midY = r.getCentreY();
        const float halfH = r.getHeight() * 0.5f;
        const int   w = juce::jmax (1, (int) r.getWidth());

        for (int x = 0; x < w; ++x)
        {
            const int b = juce::jlimit (0, kPeakResolution - 1,
                            (int) ((double) x / (double) w * kPeakResolution));

            const float mx = peaksMax[(size_t) b];
            const float mn = peaksMin[(size_t) b];

            const float y1 = midY - mx * halfH * 0.92f;
            const float y2 = midY - mn * halfH * 0.92f;

            g.drawLine (r.getX() + (float) x, y1,
                        r.getX() + (float) x, juce::jmax (y2, y1 + 1.0f), 1.0f);
        }
    }

    //==========================================================================
    static constexpr int kPeakResolution  = 320;   // buckets cached per clip
    static constexpr int kTextBlockHeight = 34;
    static constexpr int kHoldMs          = 650;   // matches SceneButton

    State        state { State::empty };
    juce::Colour accent { juce::Colour (0xff00d9ff) };
    juce::String clipName;
    double       bpm { 0.0 };

    std::vector<float> peaksMin, peaksMax;
    bool   hasAudio { false };
    double pulsePhase { 0.0 };
    bool   held { false };
    bool   dragHover { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckCard)
};
