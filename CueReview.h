// ============================================================================
//  CueReview.h -- checking the sections read from a guide track before they
//  replace the song's own.
//
//  Owner: "I should be able to make changes if the detection is wrong."
//  Every section the guide track suggested is a row: hear the spoken cue,
//  fix its name (a list of section names, or type any), move its bar, or
//  drop it. What the recognizer was sure of reads green; what it only
//  thought it heard reads amber; what it couldn't make out reads red, so a
//  performer knows exactly which ones to listen to. Nothing touches the song
//  until "Apply sections".
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "CueDetect.h"
#include "SectionNameField.h"   // type "v" -> Verse

namespace ezcuereview
{

struct Choice
{
    juce::String name;
    int startBar { 0 };   // 0-based
};

/** Owner: "it detects the count 1 2 3 4 but there's no option to say that
    something is a count." A row given this name is a count, not a section:
    it isn't applied. */
inline const char* const kCountChoice = "Count (not a section)";

class CueReviewDialog : public juce::Component
{
public:
    CueReviewDialog (const std::vector<ezcue::DraftSection>& drafts,
                     const juce::String& heading,
                     const juce::StringArray& suggestions,
                     std::function<void (double startSeconds, double lengthSeconds)> onHearIn,
                     std::function<void (const std::vector<Choice>&)> onApplyIn)
        : names (suggestions), onHear (std::move (onHearIn)), onApply (std::move (onApplyIn))
    {
        title.setText (heading, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));
        addAndMakeVisible (title);

        // the song starts somewhere: an Intro at bar 1 unless a cue already names bar 1
        bool haveBarOne = false;
        for (const auto& d : drafts) if (d.startBar == 0) haveBarOne = true;
        if (! haveBarOne)
        {
            ezcue::DraftSection intro;
            intro.name = "Intro";
            intro.startBar = 0;
            intro.confidence = 1.0f;
            addRow (intro, true);
        }
        int unsure = 0;
        for (const auto& d : drafts)
        {
            addRow (d, false);
            if (d.confidence < 0.5f) ++unsure;
        }

        summary.setText (juce::String ((int) drafts.size()) + (drafts.size() == 1 ? " section" : " sections") + " from the cues. "
                         + (unsure == 0 ? juce::String ("Everything was heard clearly - check and apply.")
                                        : juce::String (unsure) + (unsure == 1 ? " needs" : " need") + " a listen: press Hear, then fix the name, mark it a Count, or untick it."),
                         juce::dontSendNotification);
        summary.setFont (juce::Font (juce::FontOptions (12.5f)));
        summary.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));
        addAndMakeVisible (summary);

        rowViewport.setViewedComponent (&rowHolder, false);
        rowViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (rowViewport);

        applyButton.setButtonText ("Apply sections");
        applyButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7c5cffu));
        applyButton.onClick = [this] { finish (true); };
        cancelButton.setButtonText ("Cancel");
        cancelButton.onClick = [this] { finish (false); };
        addAndMakeVisible (applyButton);
        addAndMakeVisible (cancelButton);

        setSize (760, juce::jlimit (320, 680, 150 + rows.size() * kRowH));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0c0c17u));
        g.setColour (juce::Colour (0xff6f7099u));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)).withExtraKerningFactor (0.08f));
        auto h = headingArea;
        h.removeFromLeft (kHearW + 8);
        g.drawText ("BAR", h.removeFromLeft (kBarW + 8), juce::Justification::centredLeft, false);
        g.drawText ("SECTION", h.removeFromLeft (kNameW + 12), juce::Justification::centredLeft, false);
        g.drawText ("WHAT WAS HEARD", h.removeFromLeft (h.getWidth() - kKeepW), juce::Justification::centredLeft, false);
        g.drawText ("KEEP", h, juce::Justification::centred, false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 16);
        title.setBounds (area.removeFromTop (28));
        summary.setBounds (area.removeFromTop (22));
        area.removeFromTop (8);
        headingArea = area.removeFromTop (18);
        auto buttons = area.removeFromBottom (36);
        area.removeFromBottom (10);
        rowViewport.setBounds (area);

        const int w = rowViewport.getMaximumVisibleWidth();
        rowHolder.setSize (w, rows.size() * kRowH);
        for (int i = 0; i < rows.size(); ++i) rows[i]->setBounds (0, i * kRowH, w, kRowH);

        applyButton.setBounds (buttons.removeFromRight (150));
        buttons.removeFromRight (8);
        cancelButton.setBounds (buttons.removeFromRight (100));
    }

private:
    static constexpr int kRowH = 38, kHearW = 58, kBarW = 56, kNameW = 190, kKeepW = 48;

    struct Row : public juce::Component
    {
        Row (const ezcue::DraftSection& d, bool isAddedIntro, const juce::StringArray& names,
             std::function<void (double, double)>& hearRef)
            : draft (d), hear (hearRef), name (names, kCountChoice)
        {
            hearButton.setButtonText ("Hear");
            hearButton.setEnabled (d.cueSeconds >= 0.0 && hear != nullptr);
            hearButton.onClick = [this] { if (hear) hear (draft.cueSeconds, draft.cueLength); };
            addAndMakeVisible (hearButton);

            bar.setText (juce::String (d.startBar + 1), juce::dontSendNotification);
            bar.setInputRestrictions (4, "0123456789");
            bar.setJustification (juce::Justification::centred);
            addAndMakeVisible (bar);

            name.setText (d.name);
            name.onChange = [this] { refreshStatus(); };
            addAndMakeVisible (name);

            keep.setToggleState (true, juce::dontSendNotification);
            addAndMakeVisible (keep);

            juce::String text;
            if (isAddedIntro)                                  { text = "the start of the song"; colour = juce::Colour (0xff6f7099u); }
            else if (d.fromCount && d.heard.empty() && d.confidence < 0.5f) { text = "a count into the section - name it"; colour = juce::Colour (0xffffc933u); }
            else if (! d.heard.empty() && d.confidence >= 0.5f) { text = "heard \"" + juce::String (d.heard) + "\""; colour = juce::Colour (0xff2ee86au); }
            else if (! d.heard.empty())                        { text = "not sure - it sounded like \"" + juce::String (d.heard) + "\""; colour = juce::Colour (0xffffc933u); }
            else if (d.confidence >= 0.5f)                     { text = "matched a cue recording"; colour = juce::Colour (0xff2ee86au); }
            else                                               { text = "couldn't make it out - press Hear"; colour = juce::Colour (0xffff3b5cu); }
            heardText = text;
            heardColour = colour;
            status.setFont (juce::Font (juce::FontOptions (12.0f)));
            addAndMakeVisible (status);
            refreshStatus();
        }

        /** A row marked as a count says so, greyed; otherwise what was heard. */
        void refreshStatus()
        {
            const bool isCount = name.getText() == kCountChoice;
            colour = isCount ? juce::Colour (0xff6f7099u) : heardColour;
            status.setText (isCount ? juce::String ("a count - not added as a section") : heardText, juce::dontSendNotification);
            status.setColour (juce::Label::textColourId, colour);
            bar.setEnabled (! isCount);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (colour);
            g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 8.0f, 3.0f, (float) getHeight() - 16.0f), 1.5f);
            g.setColour (juce::Colour (0xff1b1b30u));
            g.fillRect (0, getHeight() - 1, getWidth(), 1);
        }

        void resized() override
        {
            auto a = getLocalBounds().reduced (0, 5).withTrimmedLeft (8);
            hearButton.setBounds (a.removeFromLeft (kHearW - 8));
            a.removeFromLeft (8);
            bar.setBounds (a.removeFromLeft (kBarW));
            a.removeFromLeft (8);
            name.setBounds (a.removeFromLeft (kNameW));
            a.removeFromLeft (12);
            keep.setBounds (a.removeFromRight (kKeepW).withSizeKeepingCentre (26, 26));
            status.setBounds (a);
        }

        ezcue::DraftSection draft;
        std::function<void (double, double)>& hear;
        juce::TextButton hearButton;
        juce::TextEditor bar;
        ezsections::SectionNameField name;
        juce::ToggleButton keep;
        juce::Label status;
        juce::Colour colour { 0xff6f7099u };
        juce::String heardText;
        juce::Colour heardColour { 0xff6f7099u };
    };

    void addRow (const ezcue::DraftSection& d, bool isAddedIntro)
    {
        auto* r = rows.add (new Row (d, isAddedIntro, names, onHear));
        rowHolder.addAndMakeVisible (r);
    }

    void finish (bool apply)
    {
        if (apply && onApply)
        {
            std::vector<Choice> out;
            for (auto* r : rows)
            {
                if (! r->keep.getToggleState()) continue;
                const auto n = r->name.getText().trim();
                const int b = r->bar.getText().getIntValue();
                if (n.isEmpty() || n == kCountChoice || b < 1) continue;
                bool dup = false;
                for (auto& c : out) if (c.startBar == b - 1) { c.name = n; dup = true; }   // the later row wins its bar
                if (! dup) out.push_back ({ n, b - 1 });
            }
            std::sort (out.begin(), out.end(), [] (const Choice& a, const Choice& b) { return a.startBar < b.startBar; });
            onApply (out);
        }
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (apply ? 1 : 0);
    }

    juce::StringArray names;
    std::function<void (double, double)> onHear;
    std::function<void (const std::vector<Choice>&)> onApply;
    juce::Label title, summary;
    juce::Viewport rowViewport;
    juce::Component rowHolder;
    juce::OwnedArray<Row> rows;
    juce::TextButton applyButton, cancelButton;
    juce::Rectangle<int> headingArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueReviewDialog)
};

} // namespace ezcuereview
