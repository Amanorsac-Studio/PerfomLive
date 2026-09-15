// ============================================================================
//  SectionNameField.h -- a section name box that finishes the word for you.
//
//  Owner: "can we type, for example, v and then verse comes up, so that we
//  don't have to always be scrolling through a long menu?"
//
//  Type the first letters and the rest of the first matching name is filled
//  in and highlighted: keep typing to narrow it ("b" Bridge, "bre" Break),
//  Backspace to keep only what you typed, Enter or Tab to take it. The common
//  section names are tried first, so "v" is Verse and "c" is Chorus rather
//  than whichever rarer word is shortest. The arrow still lists every name.
// ============================================================================
#pragma once

#include <JuceHeader.h>

namespace ezsections
{

class SectionNameField : public juce::Component
{
public:
    /** names: everything offered. topItem: an extra choice listed first in the
        arrow's menu (e.g. "Count (not a section)"), "" for none. */
    SectionNameField (const juce::StringArray& names, const juce::String& topItem = {})
        : topChoice (topItem)
    {
        // the order completion tries: the everyday names, then the rest
        static const char* const common[] = {
            "Intro", "Verse", "Pre-Chorus", "Chorus", "Post-Chorus", "Bridge", "Tag", "Turnaround",
            "Instrumental", "Interlude", "Refrain", "Vamp", "Outro", "Ending", "Breakdown", "Build",
            "Last Chorus", "Rap", "Solo"
        };
        for (auto* c : common) if (names.contains (c, true)) ordered.add (c);
        for (const auto& n : names) ordered.addIfNotAlreadyThere (n, true);
        if (topChoice.isNotEmpty()) ordered.addIfNotAlreadyThere (topChoice, true);
        allNames = names;

        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff14162au));
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff2b2b4du));
        editor.setColour (juce::TextEditor::textColourId, juce::Colour (0xfff2f0ffu));
        editor.setColour (juce::TextEditor::highlightColourId, juce::Colour (0xff7c5cffu).withAlpha (0.45f));
        editor.setTextToShowWhenEmpty ("type a name", juce::Colour (0xff6f7099u));
        editor.setSelectAllWhenFocused (false);
        editor.onTextChange = [this] { completeTyping(); if (onChange) onChange(); };
        editor.onReturnKey  = [this] { acceptCompletion(); if (onEnter) onEnter(); };
        editor.onFocusLost  = [this] { acceptCompletion(); };
        addAndMakeVisible (editor);

        listButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe")));   // small down arrow
        listButton.setTooltip ("Every section name");
        listButton.onClick = [this] { showList(); };
        addAndMakeVisible (listButton);
    }

    std::function<void()> onChange;
    std::function<void()> onEnter;   // after Enter has taken the filled-in name (a dialog can close on it)

    juce::String getText() const { return editor.getText(); }

    /** Puts the cursor in the box, ready to type. */
    void focusEditor() { editor.grabKeyboardFocus(); }

    void setText (const juce::String& text)
    {
        const juce::ScopedValueSetter<bool> quiet (settingText, true);
        editor.setText (text, false);
        typedLength = text.length();
        if (onChange) onChange();
    }

    void setEnabledLook (bool enabled) { editor.setEnabled (enabled); listButton.setEnabled (enabled); }

    void resized() override
    {
        auto a = getLocalBounds();
        listButton.setBounds (a.removeFromRight (26));
        a.removeFromRight (2);
        editor.setBounds (a);
    }

private:
    /** While typing forward at the end of the box, fill in the first name
        that starts with what's been typed, and highlight the added letters. */
    void completeTyping()
    {
        if (settingText) return;
        const auto text = editor.getText();
        const int len = text.length();
        const bool typedForward = len > typedLength;
        typedLength = len;
        if (! typedForward || len == 0 || editor.getCaretPosition() != len) return;

        for (const auto& candidate : ordered)
        {
            if (candidate.length() > len && candidate.startsWithIgnoreCase (text))
            {
                const juce::ScopedValueSetter<bool> quiet (settingText, true);
                // keep the letters as typed; add the rest of the name
                editor.setText (text + candidate.substring (len), false);
                editor.setHighlightedRegion ({ len, candidate.length() });
                return;
            }
        }
    }

    /** Enter / leaving the box: take the filled-in name as it stands. */
    void acceptCompletion()
    {
        const auto text = editor.getText().trim();
        // match a known name's capitalisation when the letters are the same
        for (const auto& candidate : ordered)
            if (candidate.equalsIgnoreCase (text) && candidate != text) { setText (candidate); break; }
        const int end = editor.getTotalNumChars();
        editor.setHighlightedRegion ({ end, end });
        editor.setCaretPosition (end);
        typedLength = end;
    }

    void showList()
    {
        juce::PopupMenu m;
        if (topChoice.isNotEmpty()) { m.addItem (1, topChoice, true, editor.getText() == topChoice); m.addSeparator(); }
        for (int i = 0; i < allNames.size(); ++i) m.addItem (100 + i, allNames[i], true, editor.getText() == allNames[i]);
        juce::Component::SafePointer<SectionNameField> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()),
                         [safe] (int r)
                         {
                             if (safe == nullptr || r <= 0) return;
                             safe->setText (r == 1 ? safe->topChoice : safe->allNames[r - 100]);
                         });
    }

    juce::StringArray allNames, ordered;
    juce::String topChoice;
    juce::TextEditor editor;
    juce::TextButton listButton;
    bool settingText { false };
    int typedLength { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionNameField)
};

} // namespace ezsections
