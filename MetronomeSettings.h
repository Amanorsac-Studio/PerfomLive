// ============================================================================
//  MetronomeSettings.h -- Settings > Metronome, and where those choices live.
//
//  Owner: "let's work on the metronome settings... we should have different
//  metronome settings." The click itself is ClickEngine.h; this is the tab
//  that sets it, and the app-wide storage (the same metronome in every
//  project, like a musician's own click).
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "ClickEngine.h"

namespace ezclick
{

/** Count-in choices that sit beside the click sound. */
struct CountInParams
{
    std::atomic<int>  barsWhenUnset { 1 };      // a song whose count-in is "none set": 0, 1, 2 or 4 bars
    std::atomic<bool> evenWithClickOff { true };// the count-in clicks even when the song's click is off
};

inline void loadSettings (juce::PropertiesFile& props, ClickParams& p, CountInParams& c)
{
    p.sound       = juce::jlimit (0, kNumSounds - 1, props.getIntValue ("metronomeSound", (int) Sound::classic));
    p.level       = (float) juce::jlimit (0.0, 1.5, props.getDoubleValue ("metronomeLevel", 1.0));
    p.accent      = props.getBoolValue ("metronomeAccent", true);
    p.accentLevel = (float) juce::jlimit (0.0, 1.0, props.getDoubleValue ("metronomeAccentLevel", 1.0));
    p.beatLevel   = (float) juce::jlimit (0.0, 1.0, props.getDoubleValue ("metronomeBeatLevel", 0.62));
    p.subdivision = juce::jlimit (1, 4, props.getIntValue ("metronomeSubdivision", 1));
    p.subLevel    = (float) juce::jlimit (0.0, 1.0, props.getDoubleValue ("metronomeSubLevel", 0.35));
    const int bars = props.getIntValue ("countInBarsWhenUnset", 1);
    c.barsWhenUnset    = (bars == 0 || bars == 1 || bars == 2 || bars == 4) ? bars : 1;
    c.evenWithClickOff = props.getBoolValue ("countInEvenWithClickOff", true);
}

inline void saveSettings (juce::PropertiesFile& props, const ClickParams& p, const CountInParams& c)
{
    props.setValue ("metronomeSound", p.sound.load());
    props.setValue ("metronomeLevel", (double) p.level.load());
    props.setValue ("metronomeAccent", p.accent.load());
    props.setValue ("metronomeAccentLevel", (double) p.accentLevel.load());
    props.setValue ("metronomeBeatLevel", (double) p.beatLevel.load());
    props.setValue ("metronomeSubdivision", p.subdivision.load());
    props.setValue ("metronomeSubLevel", (double) p.subLevel.load());
    props.setValue ("countInBarsWhenUnset", c.barsWhenUnset.load());
    props.setValue ("countInEvenWithClickOff", c.evenWithClickOff.load());
    props.saveIfNeeded();
}

//==============================================================================
class MetronomeTab final : public juce::Component
{
public:
    /** onChanged: save. onPreview: play one bar of the click as it is now. */
    MetronomeTab (ClickParams& p, CountInParams& c, std::function<void()> onChangedIn, std::function<void()> onPreviewIn)
        : params (p), countIn (c), onChanged (std::move (onChangedIn)), onPreview (std::move (onPreviewIn))
    {
        auto label = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (13.0f)));
            l.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));
            addAndMakeVisible (l);
        };
        label (soundLabel, "Sound");
        label (levelLabel, "Click volume");
        label (accentLabel, "Accent beat one");
        label (accentLevelLabel, "Beat one");
        label (beatLevelLabel, "Other beats");
        label (subLabel, "Subdivide each beat");
        label (subLevelLabel, "Subdivision volume");
        label (countInLabel, "Count-in for songs without their own");
        label (countInClickLabel, "Count in even when a song's click is off");

        for (int s = 0; s < kNumSounds; ++s) soundBox.addItem (soundName ((Sound) s), s + 1);
        soundBox.setSelectedId (params.sound.load() + 1, juce::dontSendNotification);
        soundBox.onChange = [this] { params.sound = soundBox.getSelectedId() - 1; changed(); if (onPreview) onPreview(); };

        previewButton.setButtonText ("Play a bar");
        previewButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7c5cffu));
        previewButton.onClick = [this] { if (onPreview) onPreview(); };

        subBox.addItem ("Off -- beats only", 1);
        subBox.addItem ("Eighths (2 per beat)", 2);
        subBox.addItem ("Triplets (3 per beat)", 3);
        subBox.addItem ("Sixteenths (4 per beat)", 4);
        subBox.setSelectedId (params.subdivision.load(), juce::dontSendNotification);
        subBox.onChange = [this] { params.subdivision = subBox.getSelectedId(); refreshEnabled(); changed(); };

        countInBox.addItem ("No count-in", 1);
        countInBox.addItem ("1 bar", 2);
        countInBox.addItem ("2 bars", 3);
        countInBox.addItem ("4 bars", 5);
        countInBox.setSelectedId (countIn.barsWhenUnset.load() + 1, juce::dontSendNotification);
        countInBox.onChange = [this] { countIn.barsWhenUnset = countInBox.getSelectedId() - 1; changed(); };

        accentToggle.setToggleState (params.accent.load(), juce::dontSendNotification);
        accentToggle.onClick = [this] { params.accent = accentToggle.getToggleState(); refreshEnabled(); changed(); };
        countInClickToggle.setToggleState (countIn.evenWithClickOff.load(), juce::dontSendNotification);
        countInClickToggle.onClick = [this] { countIn.evenWithClickOff = countInClickToggle.getToggleState(); changed(); };

        setUpSlider (levelSlider, params.level.load(), 0.0, 1.5, [this] (double v) { params.level = (float) v; });
        setUpSlider (accentLevelSlider, params.accentLevel.load(), 0.0, 1.0, [this] (double v) { params.accentLevel = (float) v; });
        setUpSlider (beatLevelSlider, params.beatLevel.load(), 0.0, 1.0, [this] (double v) { params.beatLevel = (float) v; });
        setUpSlider (subLevelSlider, params.subLevel.load(), 0.0, 1.0, [this] (double v) { params.subLevel = (float) v; });

        for (auto* c2 : std::initializer_list<juce::Component*> { &soundBox, &previewButton, &subBox, &countInBox, &accentToggle, &countInClickToggle })
            addAndMakeVisible (c2);
        for (auto* b : { &soundBox, &subBox, &countInBox })
            b->setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff151527u));

        note.setText ("The Click strip in the MIXER still sets the click's place in the mix and its output. "
                      "A song's own click track, when it has one, plays instead of these sounds.",
                      juce::dontSendNotification);
        note.setFont (juce::Font (juce::FontOptions (12.0f)));
        note.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));
        note.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (note);

        refreshEnabled();
        setSize (640, 480);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff0c0c17u)); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18, 14);
        auto row = [&r] (int h = 30) { auto x = r.removeFromTop (h); r.removeFromTop (6); return x; };
        const int labelW = 260;

        auto a = row();
        soundLabel.setBounds (a.removeFromLeft (labelW));
        previewButton.setBounds (a.removeFromRight (110));
        a.removeFromRight (8);
        soundBox.setBounds (a);

        a = row(); levelLabel.setBounds (a.removeFromLeft (labelW)); levelSlider.setBounds (a);
        r.removeFromTop (6);
        a = row(); accentLabel.setBounds (a.removeFromLeft (labelW)); accentToggle.setBounds (a.removeFromLeft (40));
        a = row(); accentLevelLabel.setBounds (a.removeFromLeft (labelW).withTrimmedLeft (18)); accentLevelSlider.setBounds (a);
        a = row(); beatLevelLabel.setBounds (a.removeFromLeft (labelW).withTrimmedLeft (18)); beatLevelSlider.setBounds (a);
        r.removeFromTop (6);
        a = row(); subLabel.setBounds (a.removeFromLeft (labelW)); subBox.setBounds (a);
        a = row(); subLevelLabel.setBounds (a.removeFromLeft (labelW).withTrimmedLeft (18)); subLevelSlider.setBounds (a);
        r.removeFromTop (6);
        a = row(); countInLabel.setBounds (a.removeFromLeft (labelW)); countInBox.setBounds (a);
        a = row(); countInClickLabel.setBounds (a.removeFromLeft (labelW)); countInClickToggle.setBounds (a.removeFromLeft (40));
        r.removeFromTop (8);
        note.setBounds (r.removeFromTop (40));
    }

private:
    void setUpSlider (juce::Slider& s, double value, double lo, double hi, std::function<void (double)> set)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setRange (lo, hi, 0.01);
        s.setValue (value, juce::dontSendNotification);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
        s.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
        s.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue() / 100.0; };
        s.setColour (juce::Slider::thumbColourId, juce::Colour (0xff7c5cffu));
        s.setColour (juce::Slider::trackColourId, juce::Colour (0xff7c5cffu).withAlpha (0.6f));
        s.onValueChange = [this, &s, set] { set (s.getValue()); changed(); };
        s.onDragEnd = [this] { if (onPreview) onPreview(); };
        addAndMakeVisible (s);
    }

    void refreshEnabled()
    {
        accentLevelSlider.setEnabled (accentToggle.getToggleState());
        subLevelSlider.setEnabled (subBox.getSelectedId() > 1);
    }

    void changed() { if (onChanged) onChanged(); }

    ClickParams& params;
    CountInParams& countIn;
    std::function<void()> onChanged, onPreview;

    juce::Label soundLabel, levelLabel, accentLabel, accentLevelLabel, beatLevelLabel,
                subLabel, subLevelLabel, countInLabel, countInClickLabel, note;
    juce::ComboBox soundBox, subBox, countInBox;
    juce::TextButton previewButton;
    juce::ToggleButton accentToggle, countInClickToggle;
    juce::Slider levelSlider, accentLevelSlider, beatLevelSlider, subLevelSlider;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetronomeTab)
};

} // namespace ezclick
