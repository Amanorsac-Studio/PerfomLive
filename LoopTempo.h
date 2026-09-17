// ============================================================================
//  LoopTempo.h -- telling the app what tempo a loop is at.
//
//  Owner: "if I drag in just one loop instead of stems the workflow becomes
//  very difficult... detect tempo and things don't have those options."
//
//  TempoField    an original-tempo box with Detect (TempoDetect.h, on a
//                worker thread), halve, double and Tap.
//  LoopDialog    what a single dropped loop asks: its tempo, whether it
//                repeats (loop) or plays once (like a stem), and whether it
//                should play at the song's current tempo.
//  detectFileTempo  reads a file and detects its tempo off the message thread.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "TempoDetect.h"

namespace ezloop
{

namespace colours
{
    inline const juce::Colour ground { 0xff0c0c17u };
    inline const juce::Colour card   { 0xff151527u };
    inline const juce::Colour border { 0xff2b2b4du };
    inline const juce::Colour text   { 0xfff2f0ffu };
    inline const juce::Colour dim    { 0xffa3a6ccu };
    inline const juce::Colour accent { 0xff7c5cffu };
    inline const juce::Colour good   { 0xff2ee86au };
    inline const juce::Colour warn   { 0xffffc933u };
}

/** Reads up to 95 s of `file` (every channel) and detects its tempo on a
    worker thread; `done` runs on the message thread. */
inline void detectFileTempo (const juce::File& file, std::function<void (eztempo::Result)> done)
{
    juce::Thread::launch ([file, done]
    {
        eztempo::Result result;
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> reader { fm.createReaderFor (file) })
        {
            const double sr = reader->sampleRate;
            const int total = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (sr * 95.0));
            const int channels = juce::jmax (1, (int) reader->numChannels);
            if (total > 0 && sr > 0.0)
            {
                juce::AudioBuffer<float> buffer (channels, total);
                reader->read (&buffer, 0, total, 0, true, true);
                std::vector<float> mono ((size_t) total, 0.0f);
                for (int c = 0; c < channels; ++c)
                {
                    const auto* p = buffer.getReadPointer (c);
                    for (int i = 0; i < total; ++i) mono[(size_t) i] += p[i] / (float) channels;
                }
                result = eztempo::detect (mono.data(), total, sr, (double) reader->lengthInSamples / sr);
            }
        }
        juce::MessageManager::callAsync ([done, result] { if (done) done (result); });
    });
}

inline void styleButton (juce::TextButton& b, bool primary = false)
{
    b.setColour (juce::TextButton::buttonColourId, primary ? colours::accent : colours::card);
    b.setColour (juce::TextButton::textColourOffId, colours::text);
}

//==============================================================================
class TempoField final : public juce::Component
{
public:
    TempoField()
    {
        value.setInputRestrictions (6, "0123456789.");
        value.setJustification (juce::Justification::centred);
        value.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
        value.setColour (juce::TextEditor::backgroundColourId, colours::card);
        value.setColour (juce::TextEditor::outlineColourId, colours::border);
        value.setColour (juce::TextEditor::textColourId, colours::text);
        value.setTextToShowWhenEmpty ("BPM", colours::dim);
        value.onTextChange = [this] { status.setText ({}, juce::dontSendNotification); if (onChange) onChange(); };
        addAndMakeVisible (value);

        for (auto* b : { &detectButton, &halfButton, &doubleButton, &tapButton })
        {
            styleButton (*b);
            addAndMakeVisible (b);
        }
        detectButton.setButtonText ("Detect");
        detectButton.setTooltip ("Listen to the audio and work out its tempo");
        detectButton.onClick = [this] { detect(); };
        halfButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\xb7 2")));
        halfButton.setTooltip ("Half: the same groove counted twice as slow");
        halfButton.onClick = [this] { scale (0.5); };
        doubleButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97 2")));
        doubleButton.setTooltip ("Double: the same groove counted twice as fast");
        doubleButton.onClick = [this] { scale (2.0); };
        tapButton.setButtonText ("Tap");
        tapButton.setTooltip ("Tap along with the beat, four taps or more");
        tapButton.onClick = [this] { tap(); };

        status.setFont (juce::Font (juce::FontOptions (12.5f)));
        status.setColour (juce::Label::textColourId, colours::dim);
        addAndMakeVisible (status);

        setSize (420, 72);
    }

    ~TempoField() override { alive->store (false); }

    std::function<void()> onChange;

    /** The file Detect listens to (none: Detect is off). */
    void setFile (const juce::File& f) { file = f; detectButton.setEnabled (file.existsAsFile()); }

    double getBpm() const { return value.getText().getDoubleValue(); }

    void setBpm (double bpm, const juce::String& why = {})
    {
        value.setText (bpm > 0.0 ? format (bpm) : juce::String(), juce::dontSendNotification);
        status.setText (why, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, colours::dim);
        if (onChange) onChange();
    }

    void detect()
    {
        if (! file.existsAsFile() || listening) return;
        listening = true;
        detectButton.setEnabled (false);
        status.setText ("Listening...", juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, colours::dim);
        auto flag = alive;
        juce::Component::SafePointer<TempoField> safe (this);
        detectFileTempo (file, [safe, flag] (eztempo::Result r)
        {
            if (! flag->load() || safe == nullptr) return;
            safe->listening = false;
            safe->detectButton.setEnabled (true);
            if (r.bpm <= 0.0)
            {
                safe->status.setText ("No steady beat heard -- type the tempo or Tap along", juce::dontSendNotification);
                safe->status.setColour (juce::Label::textColourId, colours::warn);
                return;
            }
            safe->value.setText (format (r.bpm), juce::dontSendNotification);
            const bool sure = r.confidence >= 0.35;
            juce::String text (sure ? "Heard " : "Probably ");
            text << format (r.bpm) << " BPM";
            if (! sure)
                text << " -- if it feels wrong, try " << juce::String (juce::CharPointer_UTF8 ("\xc3\xb7")) << "2 / "
                     << juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) << "2 or Tap";
            safe->status.setText (text, juce::dontSendNotification);
            safe->status.setColour (juce::Label::textColourId, sure ? colours::good : colours::warn);
            if (safe->onChange) safe->onChange();
        });
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto row = r.removeFromTop (38);
        value.setBounds (row.removeFromLeft (110));
        row.removeFromLeft (8);
        const int w = (row.getWidth() - 3 * 6) / 4;
        for (auto* b : { &detectButton, &halfButton, &doubleButton, &tapButton })
        {
            b->setBounds (row.removeFromLeft (w).reduced (0, 3));
            row.removeFromLeft (6);
        }
        r.removeFromTop (6);
        status.setBounds (r.removeFromTop (22));
    }

private:
    static juce::String format (double bpm)
    {
        return std::fabs (bpm - std::round (bpm)) < 0.005 ? juce::String ((int) std::round (bpm)) : juce::String (bpm, 2);
    }

    void scale (double factor)
    {
        const double v = getBpm();
        if (v <= 0.0) return;
        const double next = v * factor;
        if (next < 20.0 || next > 400.0) return;
        setBpm (next, factor < 1.0 ? "Halved" : "Doubled");
    }

    void tap()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (! taps.empty() && now - taps.back() > 2000.0) taps.clear();   // a pause starts a new count
        taps.push_back (now);
        if (taps.size() > 12) taps.erase (taps.begin());
        if (taps.size() < 2) { status.setText ("Keep tapping...", juce::dontSendNotification); return; }
        const double avg = (taps.back() - taps.front()) / (double) (taps.size() - 1);
        const double bpm = std::round (60000.0 / avg * 10.0) / 10.0;
        value.setText (format (bpm), juce::dontSendNotification);
        status.setText (taps.size() < 4 ? juce::String ("Keep tapping...")
                                        : "Tapped " + format (bpm) + " BPM (" + juce::String ((int) taps.size()) + " taps)",
                        juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, colours::dim);
        if (onChange) onChange();
    }

    juce::TextEditor value;
    juce::TextButton detectButton, halfButton, doubleButton, tapButton;
    juce::Label status;
    juce::File file;
    std::vector<double> taps;
    bool listening { false };
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };
};

//==============================================================================
/** What one dropped loop asks before it lands on a row. */
class LoopDialog final : public juce::Component
{
public:
    struct Choice
    {
        double bpm { 0.0 };         // 0 = not known (the loop plays as recorded)
        bool   repeats { true };    // loop mode; false = plays once, like a stem
        bool   playAtSongTempo { false };
    };

    /** hintBpm: a tempo already known (file name, tag, LIBRARY) -- 0 = detect. */
    LoopDialog (const juce::File& file, const juce::String& rowName, double hintBpm, const juce::String& hintSource,
                double songTempo, std::function<void (Choice)> onDoneIn)
        : onDone (std::move (onDoneIn)), songBpm (songTempo)
    {
        title.setText ("Add a loop to " + rowName, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, colours::text);
        addAndMakeVisible (title);

        fileLabel.setText (file.getFileName(), juce::dontSendNotification);
        fileLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
        fileLabel.setColour (juce::Label::textColourId, colours::dim);
        addAndMakeVisible (fileLabel);

        for (auto* l : { &tempoLabel, &playsLabel })
        {
            l->setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            l->setColour (juce::Label::textColourId, colours::text);
            addAndMakeVisible (l);
        }
        tempoLabel.setText ("Its tempo", juce::dontSendNotification);
        playsLabel.setText ("It plays", juce::dontSendNotification);

        tempo.setFile (file);
        tempo.onChange = [this] { refresh(); };
        addAndMakeVisible (tempo);

        plays.addItem ("Round and round (loop, cut to whole bars)", 1);
        plays.addItem ("Once through (like a stem)", 2);
        const bool shortFile = durationOf (file) <= 90.0;
        plays.setSelectedId (shortFile ? 1 : 2, juce::dontSendNotification);
        plays.setColour (juce::ComboBox::backgroundColourId, colours::card);
        addAndMakeVisible (plays);

        followSong.setColour (juce::ToggleButton::textColourId, colours::text);
        followSong.setToggleState (songBpm > 0.0, juce::dontSendNotification);
        addAndMakeVisible (followSong);

        styleButton (addButton, true);
        addButton.setButtonText ("Add loop");
        addButton.onClick = [this] { finish (true); };
        styleButton (cancelButton);
        cancelButton.setButtonText ("Cancel");
        cancelButton.onClick = [this] { finish (false); };
        addAndMakeVisible (addButton);
        addAndMakeVisible (cancelButton);

        if (hintBpm > 0.0) tempo.setBpm (hintBpm, "From " + hintSource + " -- Detect to check it");
        else tempo.detect();
        refresh();
        setSize (500, 330);
    }

    void paint (juce::Graphics& g) override { g.fillAll (colours::ground); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 16);
        title.setBounds (r.removeFromTop (26));
        fileLabel.setBounds (r.removeFromTop (20));
        r.removeFromTop (12);
        tempoLabel.setBounds (r.removeFromTop (20));
        tempo.setBounds (r.removeFromTop (70));
        r.removeFromTop (8);
        playsLabel.setBounds (r.removeFromTop (20));
        plays.setBounds (r.removeFromTop (28));
        r.removeFromTop (8);
        followSong.setBounds (r.removeFromTop (26));
        auto buttons = r.removeFromBottom (34);
        addButton.setBounds (buttons.removeFromRight (120));
        buttons.removeFromRight (8);
        cancelButton.setBounds (buttons.removeFromRight (100));
    }

private:
    static double durationOf (const juce::File& f)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> reader { fm.createReaderFor (f) })
            return reader->sampleRate > 0.0 ? (double) reader->lengthInSamples / reader->sampleRate : 0.0;
        return 0.0;
    }

    void refresh()
    {
        const double bpm = tempo.getBpm();
        const bool canFollow = bpm > 0.0 && songBpm > 0.0;
        followSong.setEnabled (canFollow);
        followSong.setButtonText (songBpm <= 0.0 ? juce::String ("Play at the song's tempo")
                                  : "Play at the song's tempo (" + juce::String (songBpm, 1) + " BPM)"
                                    + (bpm > 0.0 && std::fabs (bpm - songBpm) > 0.05
                                           ? juce::String (" -- stretched from ") + juce::String (bpm, 1) : juce::String()));
    }

    void finish (bool add)
    {
        Choice c;
        c.bpm = tempo.getBpm();
        c.repeats = plays.getSelectedId() == 1;
        c.playAtSongTempo = followSong.isEnabled() && followSong.getToggleState();
        auto done = onDone;   // a copy: the dialog is gone after this
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (add ? 1 : 0);
        if (add && done) done (c);
    }

    std::function<void (Choice)> onDone;
    double songBpm;
    juce::Label title, fileLabel, tempoLabel, playsLabel;
    TempoField tempo;
    juce::ComboBox plays;
    juce::ToggleButton followSong;
    juce::TextButton addButton, cancelButton;
};

/** Opens LoopDialog as its own window. */
inline void showLoopDialog (const juce::File& file, const juce::String& rowName, double hintBpm, const juce::String& hintSource,
                            double songTempo, std::function<void (LoopDialog::Choice)> onDone)
{
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Add a loop";
    o.content.setOwned (new LoopDialog (file, rowName, hintBpm, hintSource, songTempo, std::move (onDone)));
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

} // namespace ezloop
