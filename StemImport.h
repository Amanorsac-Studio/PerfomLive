// ============================================================================
//  StemImport.h -- the window that opens when a set of stems lands on a row.
//
//  Owner: "when we are importing we should be able to say okay we are
//  importing stems, then you ask: does this stem have a click? does this
//  stem have a guide? ... read the names of the elements and tell what it
//  is, and put them in the decks in a way that makes sense ... maximum they
//  can import is eight, and the click and guide go to a separate place."
//
//  planStemImport() reads the file names and proposes a role for each: one
//  of the row's eight decks (ordered drums, bass, keys, guitar, synth, pads,
//  strings, brass, vocals, other), the row's CLICK, the row's GUIDE, or
//  skipped once the eight are full. StemImportDialog shows that plan with a
//  role picker per file and the song's original tempo, which can be read
//  from the click track. Nothing is loaded until OK.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "Deck.h"
#include "PlaybackView.h"   // ezplayback::stemKindFor / drawStemIcon -- the same reading of a name PLAYBACK uses

namespace ezstems
{

enum Role : int { kSkip = -1, kClick = 100, kGuide = 101 };   // 0..kNumLayers-1 = that deck

struct Item
{
    juce::File   file;
    juce::String assetId;      // the Library entry it came from, if any
    juce::String name;         // what the performer sees
    ezplayback::StemKind kind { ezplayback::StemKind::other };
    int role { kSkip };
};

struct Plan
{
    std::vector<Item> items;
    double tempo { 0.0 };      // the song's original tempo, 0 = not known
    juce::String meter;        // the song's time signature (meterChoices()), "" = the row's group
};

/** A short display name: the file name without extension, and without a
    song prefix ("Grace 72BPM - Drums" -> "Drums"). */
inline juce::String stemDisplayName (const juce::File& f)
{
    auto n = f.getFileNameWithoutExtension();
    if (n.contains (" - ")) n = n.fromLastOccurrenceOf (" - ", false, false);
    return n.trim();
}

inline bool nameSaysClick (const juce::String& lower)
{
    return lower.contains ("click") || lower.contains ("metro") || lower.contains ("count");
}

inline bool nameSaysGuide (const juce::String& lower)
{
    return lower.contains ("guide") || lower.contains ("cue");
}

inline int kindOrder (ezplayback::StemKind k)
{
    using K = ezplayback::StemKind;
    switch (k)
    {
        case K::drums:   return 0;  case K::bass:    return 1;  case K::keys:   return 2;
        case K::guitar:  return 3;  case K::synth:   return 4;  case K::pads:   return 5;
        case K::strings: return 6;  case K::brass:   return 7;  case K::vocals: return 8;
        default:         return 9;
    }
}

/** Reads the names and proposes roles. `assetIds` may be empty or parallel
    to `files`. The first click-named file is the click, the first guide-
    named file is the guide; the rest fill decks 1..8 in instrument order,
    and anything beyond eight is skipped. */
inline Plan planStemImport (const juce::Array<juce::File>& files, const juce::StringArray& assetIds = {}, double tempoHint = 0.0)
{
    Plan plan;
    plan.tempo = tempoHint;
    for (int i = 0; i < files.size(); ++i)
    {
        Item it;
        it.file = files[i];
        if (i < assetIds.size()) it.assetId = assetIds[i];
        it.name = stemDisplayName (it.file);
        it.kind = ezplayback::stemKindFor (it.name);
        plan.items.push_back (it);
    }

    bool haveClick = false, haveGuide = false;
    std::vector<size_t> instruments;
    for (size_t i = 0; i < plan.items.size(); ++i)
    {
        auto& it = plan.items[i];
        const auto lower = it.file.getFileNameWithoutExtension().toLowerCase();
        if (! haveClick && nameSaysClick (lower))      { it.role = kClick; haveClick = true; }
        else if (! haveGuide && nameSaysGuide (lower)) { it.role = kGuide; haveGuide = true; }
        else instruments.push_back (i);
    }

    // instrument order, ties keep the file order (stable)
    std::stable_sort (instruments.begin(), instruments.end(), [&plan] (size_t a, size_t b)
                      { return kindOrder (plan.items[a].kind) < kindOrder (plan.items[b].kind); });
    int deck = 0;
    for (size_t i : instruments)
        plan.items[i].role = deck < ezdeck::kNumLayers ? deck++ : (int) kSkip;
    return plan;
}

//==============================================================================
//  Owner: "songs have different time signatures and the app should
//  accommodate that." The meters a song can be in, and how many beats (the
//  counted pulse -- what the click plays) make one of its bars.
//==============================================================================
struct Meter { const char* name; int beats; };

inline const std::vector<Meter>& meterChoices()
{
    static const std::vector<Meter> m = {
        { "4/4", 4 }, { "3/4", 3 }, { "2/4", 2 }, { "6/8", 6 }, { "6/8 in 2", 2 }, { "5/4", 5 },
        { "7/8", 7 }, { "9/8", 9 }, { "12/8", 12 }, { "12/8 in 4", 4 }, { "4/4 in 8", 8 }, { "7/4", 7 }
    };
    return m;
}

/** 0 when `name` isn't one of meterChoices(). */
inline int beatsForMeter (const juce::String& name)
{
    for (const auto& m : meterChoices()) if (name == m.name) return m.beats;
    return 0;
}

/** The usual meter for a count of beats heard in a click ("" when none fits). */
inline juce::String meterForBeats (int beats)
{
    for (const auto& m : meterChoices()) if (m.beats == beats) return m.name;
    return {};
}

inline juce::String roleName (int role)
{
    if (role == kClick) return "Click";
    if (role == kGuide) return "Guide";
    if (role == kSkip)  return "Skip";
    return "Deck " + juce::String (role + 1);
}

//==============================================================================
class StemImportDialog : public juce::Component
{
public:
    /** readTempoFromClick: decodes a click file and returns its tempo (0 if
        none heard). onApply gets the final plan; onCancel nothing. */
    StemImportDialog (Plan initial, juce::String rowLabel,
                      std::function<ezcue::ClickReading (const juce::File&)> readTempoFromClick,
                      std::function<void (const Plan&)> onApply)
        : plan (std::move (initial)), readTempo (std::move (readTempoFromClick)), apply (std::move (onApply))
    {
        title.setText ("Stems for " + rowLabel, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));
        addAndMakeVisible (title);

        hint.setText ("Each file's job was read from its name - change any of them. The click and guide "
                      "have their own place beside the row and don't use a deck.", juce::dontSendNotification);
        hint.setFont (juce::Font (juce::FontOptions (12.0f)));
        hint.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));
        addAndMakeVisible (hint);

        for (size_t i = 0; i < plan.items.size(); ++i)
        {
            auto* row = rows.add (new Row (plan.items[i]));
            row->role.onChange = [this, i, row]
            {
                plan.items[i].role = roleFromId (row->role.getSelectedId());
                refreshWarnings();
            };
            addAndMakeVisible (row);
        }

        tempoLabel.setText ("Original tempo (BPM)", juce::dontSendNotification);
        tempoLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));
        addAndMakeVisible (tempoLabel);
        tempoField.setText (plan.tempo > 0.0 ? juce::String (plan.tempo, 1) : juce::String());
        tempoField.setTextToShowWhenEmpty ("not set - the stems play as they are", juce::Colour (0xff6f7099u));
        tempoField.setInputRestrictions (6, "0123456789.");
        addAndMakeVisible (tempoField);

        meterLabel.setText ("Time signature", juce::dontSendNotification);
        meterLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));
        addAndMakeVisible (meterLabel);
        meterBox.addItem ("Same as the row's group", 1);
        {
            int id = 2;
            for (const auto& m : meterChoices()) meterBox.addItem (m.name, id++);
        }
        meterBox.setSelectedId (1, juce::dontSendNotification);
        if (plan.meter.isNotEmpty()) meterBox.setText (plan.meter, juce::dontSendNotification);
        addAndMakeVisible (meterBox);

        readClickButton.setButtonText ("Read tempo & time from click");
        readClickButton.onClick = [this] { readTempoNow(); };
        addAndMakeVisible (readClickButton);

        warning.setColour (juce::Label::textColourId, juce::Colour (0xffffc933u));
        warning.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (warning);

        okButton.setButtonText ("Load stems");
        okButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7c5cffu));
        okButton.onClick = [this] { finish (true); };
        cancelButton.setButtonText ("Cancel");
        cancelButton.onClick = [this] { finish (false); };
        addAndMakeVisible (okButton);
        addAndMakeVisible (cancelButton);

        refreshWarnings();
        setSize (640, 150 + 34 * (int) plan.items.size() + 160);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0c0c17u));
        // column headings
        g.setColour (juce::Colour (0xff6f7099u));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)).withExtraKerningFactor (0.08f));
        auto h = headingArea;
        g.drawText ("FILE", h.removeFromLeft (h.getWidth() - 150), juce::Justification::centredLeft, false);
        g.drawText ("GOES TO", h, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 16);
        title.setBounds (area.removeFromTop (28));
        hint.setBounds (area.removeFromTop (36));
        area.removeFromTop (6);
        headingArea = area.removeFromTop (16).withTrimmedLeft (36);
        for (auto* r : rows) { r->setBounds (area.removeFromTop (34)); }
        area.removeFromTop (14);

        auto tempoRow = area.removeFromTop (30);
        tempoLabel.setBounds (tempoRow.removeFromLeft (150));
        tempoField.setBounds (tempoRow.removeFromLeft (110));
        tempoRow.removeFromLeft (10);
        readClickButton.setBounds (tempoRow.removeFromLeft (230));

        area.removeFromTop (6);
        auto meterRow = area.removeFromTop (30);
        meterLabel.setBounds (meterRow.removeFromLeft (150));
        meterBox.setBounds (meterRow.removeFromLeft (180));

        area.removeFromTop (8);
        warning.setBounds (area.removeFromTop (20));

        auto buttons = area.removeFromBottom (36);
        okButton.setBounds (buttons.removeFromRight (130));
        buttons.removeFromRight (8);
        cancelButton.setBounds (buttons.removeFromRight (100));
    }

private:
    static int idFromRole (int role)   { return role == kSkip ? 1 : role == kClick ? 2 : role == kGuide ? 3 : 10 + role; }
    static int roleFromId (int id)     { return id == 1 ? (int) kSkip : id == 2 ? (int) kClick : id == 3 ? (int) kGuide : id - 10; }

    struct Row : public juce::Component
    {
        explicit Row (const Item& it) : item (it)
        {
            name.setText (it.name, juce::dontSendNotification);
            name.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));
            name.setFont (juce::Font (juce::FontOptions (13.0f)));
            addAndMakeVisible (name);
            for (int d = 0; d < ezdeck::kNumLayers; ++d) role.addItem ("Deck " + juce::String (d + 1), 10 + d);
            role.addSeparator();
            role.addItem ("Click", 2);
            role.addItem ("Guide", 3);
            role.addItem ("Skip", 1);
            role.setSelectedId (idFromRole (it.role), juce::dontSendNotification);
            addAndMakeVisible (role);
        }
        void paint (juce::Graphics& g) override
        {
            auto icon = getLocalBounds().removeFromLeft (28).reduced (4).toFloat();
            ezplayback::drawStemIcon (g, item.kind, icon, juce::Colour (0xff7c5cffu));
        }
        void resized() override
        {
            auto a = getLocalBounds();
            role.setBounds (a.removeFromRight (150).reduced (0, 4));
            a.removeFromLeft (36);
            name.setBounds (a);
        }
        Item item;
        juce::Label name;
        juce::ComboBox role;
    };

    void refreshWarnings()
    {
        int decks = 0, clicks = 0, guides = 0;
        std::vector<int> used;
        bool clash = false;
        for (const auto& it : plan.items)
        {
            if (it.role == kClick) ++clicks;
            else if (it.role == kGuide) ++guides;
            else if (it.role >= 0)
            {
                ++decks;
                if (std::find (used.begin(), used.end(), it.role) != used.end()) clash = true;
                used.push_back (it.role);
            }
        }
        juce::String w;
        if (clash) w = "Two files are going to the same deck - the later one would replace the earlier.";
        else if (clicks > 1 || guides > 1) w = "Only one click and one guide can be used - the extra one will be ignored.";
        else if (decks == 0 && clicks == 0 && guides == 0) w = "Nothing is being loaded.";
        warning.setText (w, juce::dontSendNotification);
        readClickButton.setEnabled (clicks > 0 && readTempo != nullptr);
    }

    void readTempoNow()
    {
        for (const auto& it : plan.items)
            if (it.role == kClick)
            {
                const auto reading = readTempo ? readTempo (it.file) : ezcue::ClickReading();
                if (reading.bpm > 0.0)
                {
                    // alternating clicks are eighth notes when the file's own name says the song is half that
                    const double named = ezcue::tempoFromName (it.file.getFullPathName().toStdString());
                    const bool eighths = reading.alternating && named > 0.0 && std::abs (named * 2.0 - reading.clicksPerMinute) < 1.0;
                    tempoField.setText (juce::String (eighths ? named : reading.bpm, 1));
                    const auto meter = meterForBeats (reading.beatsPerBar);
                    if (meter.isNotEmpty()) meterBox.setText (meter, juce::dontSendNotification);
                    juce::String note;
                    if (reading.alternating)
                        note = "The click alternates two sounds, " + juce::String (reading.clicksPerMinute, 0) + " a minute"
                             + (eighths ? " - eighth notes of " + juce::String (named, 0) + " BPM, as the file name says."
                                        : ": " + juce::String (reading.clicksPerMinute * 0.5, 0) + " BPM if they're eighth notes.")
                             + " It has no beat-one click, so choose the time signature.";
                    else if (meter.isEmpty())
                        note = "Tempo read. The click has no beat-one accent, so choose the time signature.";
                    warning.setText (note, juce::dontSendNotification);
                }
                else warning.setText ("No steady clicks were heard in \"" + it.name + "\" - type the tempo instead.", juce::dontSendNotification);
                return;
            }
    }

    void finish (bool ok)
    {
        if (ok)
        {
            plan.tempo = tempoField.getText().getDoubleValue();
            if (plan.tempo > 0.0) plan.tempo = juce::jlimit (1.0, 999.0, plan.tempo);
            plan.meter = meterBox.getSelectedId() <= 1 ? juce::String() : meterBox.getText();
            if (apply) apply (plan);
        }
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (ok ? 1 : 0);
    }

    Plan plan;
    std::function<ezcue::ClickReading (const juce::File&)> readTempo;
    std::function<void (const Plan&)> apply;
    juce::Label title, hint, tempoLabel, meterLabel, warning;
    juce::OwnedArray<Row> rows;
    juce::TextEditor tempoField;
    juce::ComboBox meterBox;
    juce::TextButton readClickButton, okButton, cancelButton;
    juce::Rectangle<int> headingArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemImportDialog)
};

} // namespace ezstems
