// ============================================================================
//  PluginManager.h -- the plugin scan window and Settings > Plugins.
//
//  Owner: "scanning plugins gets stuck on some plugins, we should be able to
//  skip the ones that get stuck" and "we should have a plugin manager window
//  in settings".
//
//  ScanWindow   runs a scan on its own thread and shows what is being checked
//               and for how long, with Skip this plugin and Stop scan. A plugin
//               that doesn't answer within the time limit is skipped on its
//               own (InstrumentHost.h's OutOfProcessScanner).
//  ManagerTab   the Plugins tab: the instruments PerformLive can load, the
//               plugins that were set aside and why (with Try again), the scan
//               buttons and the time limit.
//
//  Both work on ezinst::PluginLibrary and call back into the app for the
//  things the app owns (starting a scan, saving the list).
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "InstrumentHost.h"

namespace ezplugins
{

namespace colours
{
    inline const juce::Colour ground { 0xff0c0c17u };
    inline const juce::Colour card   { 0xff151527u };
    inline const juce::Colour border { 0xff2b2b4du };
    inline const juce::Colour text   { 0xfff2f0ffu };
    inline const juce::Colour dim    { 0xffa3a6ccu };
    inline const juce::Colour faint  { 0xff6f7099u };
    inline const juce::Colour accent { 0xff7c5cffu };
    inline const juce::Colour warn   { 0xffffc933u };
}

inline juce::String fileNameOf (const juce::String& fileOrIdentifier)
{
    return fileOrIdentifier.fromLastOccurrenceOf ("\\", false, false).fromLastOccurrenceOf ("/", false, false);
}

inline void styleButton (juce::TextButton& b, bool primary = false)
{
    b.setColour (juce::TextButton::buttonColourId, primary ? colours::accent : colours::card);
    b.setColour (juce::TextButton::textColourOffId, colours::text);
}

//==============================================================================
/** The scan in progress. Closing it stops the scan. */
class ScanWindow final : public juce::DocumentWindow
{
public:
    /** onFinished(stopped, filesChecked) runs on the message thread once the
        scan's thread has ended; the owner deletes the window from there. */
    ScanWindow (ezinst::PluginLibrary& lib, juce::File deadMansPedal, std::function<void (bool, int)> onFinishedIn)
        : juce::DocumentWindow ("Scanning for instruments", colours::ground, juce::DocumentWindow::closeButton),
          content (new Content (lib, std::move (deadMansPedal), std::move (onFinishedIn)))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        content->start();
    }

    ~ScanWindow() override { content->stopAndWait(); }

    void closeButtonPressed() override { content->requestStop(); }

private:
    class Content final : public juce::Component, private juce::Timer
    {
    public:
        Content (ezinst::PluginLibrary& l, juce::File pedalIn, std::function<void (bool, int)> done)
            : library (l), pedal (std::move (pedalIn)), onFinished (std::move (done)), thread (*this)
        {
            title.setText ("Checking your plugin folders for instruments", juce::dontSendNotification);
            title.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
            title.setColour (juce::Label::textColourId, colours::text);
            addAndMakeVisible (title);

            for (auto* l2 : { &current, &elapsed, &setAsideLabel })
            {
                l2->setColour (juce::Label::textColourId, colours::dim);
                l2->setFont (juce::Font (juce::FontOptions (13.0f)));
                addAndMakeVisible (l2);
            }
            current.setText ("Starting...", juce::dontSendNotification);
            setAsideLabel.setText ("Set aside this scan", juce::dontSendNotification);

            addAndMakeVisible (bar);
            bar.setColour (juce::ProgressBar::foregroundColourId, colours::accent);

            setAside.setMultiLine (true);
            setAside.setReadOnly (true);
            setAside.setCaretVisible (false);
            setAside.setColour (juce::TextEditor::backgroundColourId, colours::card);
            setAside.setColour (juce::TextEditor::outlineColourId, colours::border);
            setAside.setColour (juce::TextEditor::textColourId, colours::dim);
            setAside.setTextToShowWhenEmpty ("Nothing yet", colours::faint);
            addAndMakeVisible (setAside);

            styleButton (skipButton);
            skipButton.setButtonText ("Skip this plugin");
            skipButton.setTooltip ("Stop waiting for this plugin and move on. It is listed under Skipped in Settings > Plugins, where you can try it again.");
            skipButton.onClick = [this] { library.scanControl().skipCurrent = true; };
            addAndMakeVisible (skipButton);

            styleButton (stopButton);
            stopButton.setButtonText ("Stop scan");
            stopButton.onClick = [this] { requestStop(); };
            addAndMakeVisible (stopButton);

            setSize (560, 360);
        }

        ~Content() override { stopAndWait(); }

        void start()
        {
            knownAside = library.setAsideFiles();
            thread.startThread();
            startTimerHz (5);
        }

        void requestStop()
        {
            stopping = true;
            library.scanControl().stopAll = true;
            thread.signalThreadShouldExit();
            stopButton.setEnabled (false);
            stopButton.setButtonText ("Stopping...");
            skipButton.setEnabled (false);
        }

        void stopAndWait()
        {
            stopTimer();
            library.scanControl().stopAll = true;
            thread.stopThread (15000);   // a probe notices the stop within 50 ms
        }

        void paint (juce::Graphics& g) override { g.fillAll (colours::ground); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (20, 16);
            title.setBounds (r.removeFromTop (26));
            r.removeFromTop (6);
            current.setBounds (r.removeFromTop (20));
            elapsed.setBounds (r.removeFromTop (20));
            r.removeFromTop (6);
            bar.setBounds (r.removeFromTop (18));
            r.removeFromTop (12);
            auto buttons = r.removeFromBottom (34);
            stopButton.setBounds (buttons.removeFromRight (120));
            buttons.removeFromRight (8);
            skipButton.setBounds (buttons.removeFromRight (150));
            r.removeFromBottom (10);
            setAsideLabel.setBounds (r.removeFromTop (20));
            setAside.setBounds (r);
        }

    private:
        struct ScanThread final : public juce::Thread
        {
            explicit ScanThread (Content& o) : juce::Thread ("plugin scan"), owner (o) {}
            void run() override
            {
                const int files = owner.library.scanStandardFolders (owner.pedal,
                    [this] { return threadShouldExit(); },
                    [this] (const juce::String&, float fraction) { owner.progress = fraction; });
                const bool stopped = threadShouldExit() || owner.library.scanControl().stopAll.load();
                juce::Component::SafePointer<Content> safe (&owner);
                juce::MessageManager::callAsync ([safe, stopped, files]
                {
                    if (safe == nullptr) return;
                    // a copy: the owner may delete this window from inside the call
                    auto finished = safe->onFinished;
                    if (finished) finished (stopped, files);
                });
            }
            Content& owner;
        };

        void timerCallback() override
        {
            auto& control = library.scanControl();
            const auto file = control.currentFile();
            const auto started = control.probeStartedMs.load();
            if (file.isNotEmpty() && started != 0)
            {
                const int secs = (int) ((juce::Time::getMillisecondCounter() - started) / 1000);
                current.setText ("Checking " + fileNameOf (file), juce::dontSendNotification);
                elapsed.setText (secs < 3 ? juce::String()
                                          : juce::String (secs) + " s -- skipped automatically at "
                                              + juce::String (library.timeoutSeconds()) + " s",
                                 juce::dontSendNotification);
                elapsed.setColour (juce::Label::textColourId, secs >= 10 ? colours::warn : colours::dim);
                skipButton.setEnabled (! stopping && secs >= 2);
            }
            else
            {
                current.setText (stopping ? "Stopping..." : "Looking for plugins...", juce::dontSendNotification);
                elapsed.setText ({}, juce::dontSendNotification);
                skipButton.setEnabled (false);
            }
            progressValue = (double) progress.load();

            // new entries on the set-aside list, with why
            const auto aside = library.setAsideFiles();
            if (aside.size() != shownAside)
            {
                shownAside = aside.size();
                juce::String text;
                for (const auto& f : aside)
                    if (! knownAside.contains (f))
                        text << fileNameOf (f) << "  --  " << library.reasonSetAside (f) << "\n";
                setAside.setText (text.trimEnd(), juce::dontSendNotification);
            }
        }

        ezinst::PluginLibrary& library;
        juce::File pedal;
        std::function<void (bool, int)> onFinished;

        juce::Label title, current, elapsed, setAsideLabel;
        double progressValue { 0.0 };
        juce::ProgressBar bar { progressValue };
        juce::TextEditor setAside;
        juce::TextButton skipButton, stopButton;

        std::atomic<float> progress { 0.0f };
        juce::StringArray knownAside;
        int shownAside { -1 };
        bool stopping { false };

        ScanThread thread;   // last: stopped (in ~Content) before the rest goes
    };

    Content* content;   // owned by the window
};

//==============================================================================
/** Settings > Plugins. */
class ManagerTab final : public juce::Component,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    struct Host
    {
        std::function<void (bool rescanEverything)> startScan;
        std::function<bool()>                       isScanning;
        std::function<void()>                       saveList;
    };

    ManagerTab (ezinst::PluginLibrary& lib, Host hostIn)
        : library (lib), host (std::move (hostIn)), instrumentModel (*this), asideModel (*this)
    {
        auto heading = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, colours::text);
            addAndMakeVisible (l);
        };
        heading (instrumentsHeading, "Instruments");
        heading (asideHeading, "Skipped or failed");

        for (auto* l : { &effectsNote, &timeoutLabel, &scanState })
        {
            l->setFont (juce::Font (juce::FontOptions (12.5f)));
            l->setColour (juce::Label::textColourId, colours::dim);
            addAndMakeVisible (l);
        }
        timeoutLabel.setText ("Skip a plugin that doesn't answer within", juce::dontSendNotification);

        for (auto* list : { &instrumentList, &asideList })
        {
            list->setColour (juce::ListBox::backgroundColourId, colours::card);
            list->setColour (juce::ListBox::outlineColourId, colours::border);
            list->setOutlineThickness (1);
            list->setRowHeight (34);
            addAndMakeVisible (list);
        }
        instrumentList.setModel (&instrumentModel);
        asideList.setModel (&asideModel);
        asideList.setMultipleSelectionEnabled (true);
        instrumentList.setMultipleSelectionEnabled (true);

        styleButton (scanButton, true);
        scanButton.setButtonText ("Scan for new plugins");
        scanButton.onClick = [this] { if (host.startScan) host.startScan (false); };

        styleButton (rescanButton);
        rescanButton.setButtonText ("Rescan everything");
        rescanButton.onClick = [this]
        {
            juce::Component::SafePointer<ManagerTab> safe (this);
            auto onAnswer = [safe] (int r)
            {
                if (safe != nullptr && r == 1 && safe->host.startScan) safe->host.startScan (true);
            };
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Rescan everything?",
                "PerformLive forgets every plugin it knows, including the skipped ones, and checks all of them again. "
                "With a large plugin folder this can take several minutes.",
                "Rescan", "Cancel", this, juce::ModalCallbackFunction::create (onAnswer));
        };

        styleButton (removeButton);
        removeButton.setButtonText ("Remove from list");
        removeButton.setTooltip ("Hide these instruments from the LIVE track menu. A rescan finds them again.");
        removeButton.onClick = [this]
        {
            const auto rows = instrumentList.getSelectedRows();
            juce::Array<juce::PluginDescription> gone;
            for (int i = 0; i < rows.size(); ++i)
                if (juce::isPositiveAndBelow (rows[i], instruments.size())) gone.add (instruments[rows[i]]);
            for (const auto& d : gone) library.forget (d);
            changed();
        };

        styleButton (showButton);
        showButton.setButtonText ("Show file");
        showButton.onClick = [this]
        {
            const int row = instrumentList.getSelectedRow();
            if (juce::isPositiveAndBelow (row, instruments.size()))
                juce::File (instruments[row].fileOrIdentifier).revealToUser();
        };

        styleButton (retryButton);
        retryButton.setButtonText ("Try again");
        retryButton.setTooltip ("Check these plugins again on the next scan, and start it now.");
        retryButton.onClick = [this]
        {
            const auto rows = asideList.getSelectedRows();
            juce::StringArray files;
            for (int i = 0; i < rows.size(); ++i)
                if (juce::isPositiveAndBelow (rows[i], aside.size())) files.add (aside[rows[i]]);
            for (const auto& f : files) library.allowAgain (f);
            changed();
            if (! files.isEmpty() && host.startScan) host.startScan (false);
        };

        styleButton (retryAllButton);
        retryAllButton.setButtonText ("Try all again");
        retryAllButton.onClick = [this]
        {
            library.allowAllAgain();
            changed();
            if (host.startScan) host.startScan (false);
        };

        for (auto* b : { &scanButton, &rescanButton, &removeButton, &showButton, &retryButton, &retryAllButton })
            addAndMakeVisible (b);

        const int choices[] = { 10, 20, 30, 60, 120 };
        for (int s : choices) timeoutBox.addItem (juce::String (s) + " seconds", s);
        timeoutBox.setSelectedId (nearestChoice (library.timeoutSeconds()), juce::dontSendNotification);
        timeoutBox.onChange = [this]
        {
            library.setTimeoutSeconds (timeoutBox.getSelectedId());
            if (host.saveList) host.saveList();
        };
        timeoutBox.setColour (juce::ComboBox::backgroundColourId, colours::card);
        addAndMakeVisible (timeoutBox);

        library.list().addChangeListener (this);
        refresh();
        startTimerHz (2);
        setSize (640, 500);
    }

    ~ManagerTab() override
    {
        library.list().removeChangeListener (this);
        instrumentList.setModel (nullptr);
        asideList.setModel (nullptr);
    }

    void paint (juce::Graphics& g) override { g.fillAll (colours::ground); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 12);

        auto top = r.removeFromTop (30);
        rescanButton.setBounds (top.removeFromRight (150));
        top.removeFromRight (8);
        scanButton.setBounds (top.removeFromRight (170));
        top.removeFromRight (8);
        scanState.setBounds (top);
        r.removeFromTop (10);

        auto bottom = r.removeFromBottom (26);
        timeoutBox.setBounds (bottom.removeFromRight (130));
        bottom.removeFromRight (8);
        timeoutLabel.setBounds (bottom);
        r.removeFromBottom (8);
        effectsNote.setBounds (r.removeFromBottom (20));
        r.removeFromBottom (6);

        const int half = (r.getHeight() - 8) / 2;
        auto upper = r.removeFromTop (half);
        r.removeFromTop (8);
        auto lower = r;

        auto layoutBlock = [] (juce::Rectangle<int> area, juce::Label& heading, juce::ListBox& list,
                               juce::TextButton& a, juce::TextButton& b)
        {
            auto head = area.removeFromTop (26);
            b.setBounds (head.removeFromRight (130).reduced (0, 1));
            head.removeFromRight (6);
            a.setBounds (head.removeFromRight (130).reduced (0, 1));
            heading.setBounds (head);
            area.removeFromTop (4);
            list.setBounds (area);
        };
        layoutBlock (upper, instrumentsHeading, instrumentList, removeButton, showButton);
        layoutBlock (lower, asideHeading, asideList, retryButton, retryAllButton);
    }

private:
    struct InstrumentModel final : public juce::ListBoxModel
    {
        explicit InstrumentModel (ManagerTab& o) : owner (o) {}
        int getNumRows() override { return owner.instruments.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (! juce::isPositiveAndBelow (row, owner.instruments.size())) return;
            const auto& d = owner.instruments.getReference (row);
            if (selected) g.fillAll (colours::accent.withAlpha (0.35f));
            g.setColour (colours::text);
            g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
            g.drawText (d.name, 10, 2, w - 20, h / 2, juce::Justification::bottomLeft, true);
            g.setColour (colours::faint);
            g.setFont (juce::FontOptions (11.5f));
            g.drawText ((d.manufacturerName.isNotEmpty() ? d.manufacturerName + "  -  " : juce::String())
                            + d.pluginFormatName + "  -  " + fileNameOf (d.fileOrIdentifier),
                        10, h / 2, w - 20, h / 2 - 2, juce::Justification::topLeft, true);
        }
        ManagerTab& owner;
    };

    struct AsideModel final : public juce::ListBoxModel
    {
        explicit AsideModel (ManagerTab& o) : owner (o) {}
        int getNumRows() override { return owner.aside.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (! juce::isPositiveAndBelow (row, owner.aside.size())) return;
            const auto& file = owner.aside[row];
            if (selected) g.fillAll (colours::accent.withAlpha (0.35f));
            g.setColour (colours::text);
            g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
            g.drawText (fileNameOf (file), 10, 2, w - 20, h / 2, juce::Justification::bottomLeft, true);
            g.setColour (colours::warn.withAlpha (0.85f));
            g.setFont (juce::FontOptions (11.5f));
            g.drawText (owner.library.reasonSetAside (file) + "  -  " + file,
                        10, h / 2, w - 20, h / 2 - 2, juce::Justification::topLeft, true);
        }
        ManagerTab& owner;
    };

    static int nearestChoice (int seconds)
    {
        int best = 30;
        for (int s : { 10, 20, 30, 60, 120 })
            if (std::abs (s - seconds) < std::abs (best - seconds)) best = s;
        return best;
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void timerCallback() override
    {
        const bool scanning = host.isScanning && host.isScanning();
        if (scanning != wasScanning) { wasScanning = scanning; refresh(); }
    }

    void changed()
    {
        if (host.saveList) host.saveList();
        refresh();
    }

    void refresh()
    {
        instruments = library.instruments();
        aside = library.setAsideFiles();
        instrumentList.updateContent();
        asideList.updateContent();
        instrumentList.repaint();
        asideList.repaint();

        instrumentsHeading.setText ("Instruments (" + juce::String (instruments.size()) + ")", juce::dontSendNotification);
        asideHeading.setText ("Skipped or failed (" + juce::String (aside.size()) + ")", juce::dontSendNotification);

        const int effects = library.effectCount();
        effectsNote.setText (effects == 0 ? juce::String ("PerformLive hosts instrument plugins; effects come from its own PERFORM LIVE channel strip.")
                                          : juce::String (effects) + " effect plugin" + (effects == 1 ? "" : "s")
                                              + " ignored -- PerformLive hosts instruments; effects come from its PERFORM LIVE channel strip.",
                             juce::dontSendNotification);

        const bool scanning = host.isScanning && host.isScanning();
        scanState.setText (scanning ? "Scanning -- see the scan window" : juce::String(), juce::dontSendNotification);
        for (auto* b : { &scanButton, &rescanButton, &retryButton, &retryAllButton })
            b->setEnabled (! scanning);
        retryAllButton.setEnabled (! scanning && ! aside.isEmpty());
        instrumentList.setVisible (true);
        asideList.setVisible (true);
    }

    ezinst::PluginLibrary& library;
    Host host;

    juce::Array<juce::PluginDescription> instruments;
    juce::StringArray aside;

    InstrumentModel instrumentModel;
    AsideModel asideModel;
    juce::Label instrumentsHeading, asideHeading, effectsNote, timeoutLabel, scanState;
    juce::ListBox instrumentList, asideList;
    juce::TextButton scanButton, rescanButton, removeButton, showButton, retryButton, retryAllButton;
    juce::ComboBox timeoutBox;
    bool wasScanning { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManagerTab)
};

} // namespace ezplugins
