// ============================================================================
//  InstrumentHost.h -- VST3 instrument hosting for a column (PX-D).
//
//  The owner's ask: "deck 5 and 6 can be used for MIDI where it recognises
//  virtual MIDI plugins like Kontakt". A column can hold an instrument plugin
//  instead of a stem: the app's MIDI inputs play it, its audio goes down the
//  column's own mixer strip (level, mute, output routing like everything
//  else), and its editor opens in its own window.
//
//  Three concerns live here, each kept small:
//
//    PluginLibrary   -- what plugins exist. Scanning runs in a SEPARATE
//                       PROCESS (this same exe, relaunched with a hidden
//                       flag): a plugin that crashes while being probed takes
//                       the scanner down, not the show. Same design as every
//                       DAW, and JUCE's own AudioPluginHost.
//    InstrumentSlot  -- one column's plugin instance. The audio thread only
//                       ever sees an atomic pointer to a fully prepared
//                       instance; swapping and deleting happen on the
//                       message thread, and an old instance is freed only
//                       after the audio thread has provably let go of it.
//    HostPlayHead    -- the app's clock, so arpeggiators and tempo-synced
//                       plugins follow the song.
//
//  Needs juce_audio_processors with JUCE_PLUGINHOST_VST3=1 (the VST3 SDK
//  headers are bundled with JUCE 9; nothing to download).
// ============================================================================
#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace ezinst
{

/** The hidden command-line switch and IPC id the scanner subprocess uses. */
inline constexpr const char* kScannerProcessUID = "performlive-plugin-scanner";

//==============================================================================
//  The scanner SUBPROCESS side: this exe, started with the hidden flag, sits
//  waiting for "format + path" requests and answers with descriptions.
//==============================================================================
class ScannerSubprocess final : private juce::ChildProcessWorker,
                                private juce::AsyncUpdater
{
public:
    ScannerSubprocess() { juce::addDefaultFormatsToManager (formatManager); }
    ~ScannerSubprocess() override { cancelPendingUpdate(); }

    using juce::ChildProcessWorker::initialiseFromCommandLine;

private:
    void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override
    {
        if (mb.isEmpty()) return;
        const std::lock_guard<std::mutex> lock (mutex);
        if (const auto results = doScan (mb); ! results.isEmpty())
            sendResults (results);
        else
        {
            pendingBlocks.emplace (mb);
            triggerAsyncUpdate();
        }
    }

    // The app hung up: it finished, stopped the scan, or gave up on a plugin
    // that never answered. Leave at once -- this runs on the connection's own
    // thread, and a hung plugin may hold the message thread forever, so a
    // normal quit() would never happen and the process would stay behind.
    void handleConnectionLost() override { std::_Exit (0); }

    void handleAsyncUpdate() override
    {
        for (;;)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (pendingBlocks.empty()) return;
            sendResults (doScan (pendingBlocks.front()));
            pendingBlocks.pop();
        }
    }

    juce::OwnedArray<juce::PluginDescription> doScan (const juce::MemoryBlock& block)
    {
        juce::MemoryInputStream stream { block, false };
        const auto formatName = stream.readString();
        const auto identifier = stream.readString();

        juce::PluginDescription pd;
        pd.fileOrIdentifier = identifier;
        pd.uniqueId = pd.deprecatedUid = 0;

        juce::AudioPluginFormat* matching = nullptr;
        for (auto* f : formatManager.getFormats())
            if (f->getName() == formatName) matching = f;

        juce::OwnedArray<juce::PluginDescription> results;
        if (matching != nullptr
            && (juce::MessageManager::getInstance()->isThisTheMessageThread()
                || matching->requiresUnblockedMessageThreadDuringCreation (pd)))
            matching->findAllTypesForFile (results, identifier);
        return results;
    }

    void sendResults (const juce::OwnedArray<juce::PluginDescription>& results)
    {
        juce::XmlElement xml ("LIST");
        for (const auto* desc : results) xml.addChildElement (desc->createXml().release());
        const auto str = xml.toString();
        sendMessageToCoordinator ({ str.toRawUTF8(), str.getNumBytesAsUTF8() });
    }

    std::mutex mutex;
    std::queue<juce::MemoryBlock> pendingBlocks;
    juce::AudioPluginFormatManager formatManager;
};

//==============================================================================
//  What the person watching a scan can do about it, shared between the scan
//  window (message thread) and the scanner (the scan's own thread).
//  Owner: "scanning gets stuck on some plugins, we should be able to skip the
//  ones that get stuck."
//==============================================================================
struct ScanControl
{
    std::atomic<bool> skipCurrent { false };   // "Skip this plugin"
    std::atomic<bool> stopAll     { false };   // "Stop scan"
    std::atomic<int>  timeoutMs   { 30'000 };  // a plugin that doesn't answer in this long is skipped
    std::atomic<juce::uint32> probeStartedMs { 0 };   // 0 = not checking a plugin right now

    juce::String currentFile() const            { const juce::ScopedLock sl (lock); return current; }
    void setCurrentFile (const juce::String& f) { const juce::ScopedLock sl (lock); current = f; }

    /** Why a file was passed over ("Skipped", "Didn't answer within 30 s", ...). */
    juce::String reasonFor (const juce::String& file) const { const juce::ScopedLock sl (lock); return reasons[file]; }
    void setReason (const juce::String& file, const juce::String& why) { const juce::ScopedLock sl (lock); reasons.set (file, why); }
    void clearReason (const juce::String& file)  { const juce::ScopedLock sl (lock); reasons.remove (juce::StringRef (file)); }
    void clearReasons()                          { const juce::ScopedLock sl (lock); reasons.clear(); }
    juce::StringPairArray allReasons() const     { const juce::ScopedLock sl (lock); return reasons; }
    void setAllReasons (const juce::StringPairArray& r) { const juce::ScopedLock sl (lock); reasons = r; }

private:
    juce::CriticalSection lock;
    juce::String current;
    juce::StringPairArray reasons;
};

//==============================================================================
//  The HOST side of the scan: launches the subprocess and asks it about one
//  file at a time. If the subprocess dies, hangs past the time limit, or the
//  person skips it, that file is set aside (JUCE's blacklist, with a reason)
//  and a fresh subprocess handles the next one.
//==============================================================================
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    explicit OutOfProcessScanner (std::shared_ptr<ScanControl> c) : control (std::move (c)) {}

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        control->skipCurrent = false;
        control->setCurrentFile (fileOrIdentifier);
        control->probeStartedMs = juce::jmax<juce::uint32> (1, juce::Time::getMillisecondCounter());
        const auto outcome = addPluginDescriptions (format.getName(), fileOrIdentifier, result);
        control->probeStartedMs = 0;

        switch (outcome)
        {
            case Outcome::found:
                control->clearReason (fileOrIdentifier);
                return true;
            case Outcome::stopped:
                worker = nullptr;   // not this plugin's fault: not set aside
                return true;
            case Outcome::skipped:
                control->setReason (fileOrIdentifier, "Skipped");
                break;
            case Outcome::timedOut:
                control->setReason (fileOrIdentifier, "Didn't answer within " + juce::String (control->timeoutMs.load() / 1000) + " s");
                break;
            case Outcome::crashed:
                control->setReason (fileOrIdentifier, "Crashed while being checked");
                break;
        }
        worker = nullptr;   // the next file gets a new subprocess; this one exits (handleConnectionLost)
        return false;
    }

    void scanFinished() override { worker = nullptr; control->setCurrentFile ({}); }

private:
    enum class Outcome { found, crashed, timedOut, skipped, stopped };
    std::shared_ptr<ScanControl> control;

    class Worker final : private juce::ChildProcessCoordinator
    {
    public:
        Worker()
        {
            launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile), kScannerProcessUID, 0, 0);
        }

        enum class State { timeout, gotResult, connectionLost };
        struct Response { State state; std::unique_ptr<juce::XmlElement> xml; };

        Response getResponse()
        {
            std::unique_lock<std::mutex> lock { mutex };
            if (! condvar.wait_for (lock, std::chrono::milliseconds { 50 }, [&] { return gotResult || connectionLost; }))
                return { State::timeout, nullptr };
            const auto state = connectionLost ? State::connectionLost : State::gotResult;
            connectionLost = false;
            gotResult = false;
            return { state, std::move (description) };
        }

        using juce::ChildProcessCoordinator::sendMessageToWorker;

    private:
        void handleMessageFromWorker (const juce::MemoryBlock& mb) override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            description = juce::parseXML (mb.toString());
            gotResult = true;
            condvar.notify_one();
        }

        void handleConnectionLost() override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            connectionLost = true;
            condvar.notify_one();
        }

        std::mutex mutex;
        std::condition_variable condvar;
        std::unique_ptr<juce::XmlElement> description;
        bool connectionLost = false, gotResult = false;
    };

    Outcome addPluginDescriptions (const juce::String& formatName, const juce::String& fileOrIdentifier,
                                   juce::OwnedArray<juce::PluginDescription>& result)
    {
        if (worker == nullptr) worker = std::make_unique<Worker>();

        juce::MemoryBlock block;
        juce::MemoryOutputStream stream { block, true };
        stream.writeString (formatName);
        stream.writeString (fileOrIdentifier);
        if (! worker->sendMessageToWorker (block)) return Outcome::crashed;

        // Checked every 50 ms: the person's Skip and Stop, and the time limit.
        const auto deadline = juce::Time::getMillisecondCounter()
                              + (juce::uint32) juce::jlimit (3'000, 600'000, control->timeoutMs.load());
        for (;;)
        {
            if (shouldExit() || control->stopAll) return Outcome::stopped;
            if (control->skipCurrent.exchange (false)) return Outcome::skipped;
            if (juce::Time::getMillisecondCounter() > deadline) return Outcome::timedOut;
            const auto response = worker->getResponse();
            if (response.state == Worker::State::timeout) continue;
            if (response.xml != nullptr)
                for (const auto* item : response.xml->getChildIterator())
                {
                    auto desc = std::make_unique<juce::PluginDescription>();
                    if (desc->loadFromXml (*item)) result.add (std::move (desc));
                }
            return response.state == Worker::State::gotResult ? Outcome::found : Outcome::crashed;
        }
    }

    std::unique_ptr<Worker> worker;
};

//==============================================================================
//  What plugins exist. Persisted as XML in the app settings; scanned on
//  request (never at startup -- a scan can take minutes with a big library).
//==============================================================================
class PluginLibrary
{
public:
    PluginLibrary()
    {
        juce::addDefaultFormatsToManager (formats);
        known.setCustomScanner (std::make_unique<OutOfProcessScanner> (control));
    }

    juce::AudioPluginFormatManager& formatManager() { return formats; }
    juce::KnownPluginList& list() { return known; }
    ScanControl& scanControl() { return *control; }

    /** Scans every format's standard folders, skipping what is already known
        or set aside. progress gets the file about to be checked and the
        overall fraction. Returns how many files were checked. Runs on the
        caller's thread (never the message thread: a scan can take minutes). */
    int scanStandardFolders (const juce::File& deadMansPedal,
                             const std::function<bool()>& stopRequested,
                             const std::function<void (const juce::String&, float)>& progress)
    {
        control->stopAll = false;
        control->skipCurrent = false;
        int files = 0;
        const int numFormats = juce::jmax (1, formats.getNumFormats());
        for (int fi = 0; fi < formats.getNumFormats(); ++fi)
        {
            auto* format = formats.getFormat (fi);
            juce::PluginDirectoryScanner scanner (known, *format, format->getDefaultLocationsToSearch(), true, deadMansPedal, true);
            juce::String name;
            for (;;)
            {
                if ((stopRequested && stopRequested()) || control->stopAll) return files;
                if (progress) progress (scanner.getNextPluginFileThatWillBeScanned(), ((float) fi + scanner.getProgress()) / (float) numFormats);
                if (! scanner.scanNextFile (true, name)) break;
                ++files;
            }
        }
        if (progress) progress ({}, 1.0f);
        return files;
    }

    /** Files that were set aside (skipped, too slow, crashed), newest last. */
    juce::StringArray setAsideFiles() const { return known.getBlacklistedFiles(); }
    juce::String reasonSetAside (const juce::String& file) const
    {
        const auto why = control->reasonFor (file);
        return why.isNotEmpty() ? why : juce::String ("Couldn't be checked");
    }

    /** "Try again": the next scan checks this file once more. */
    void allowAgain (const juce::String& file) { known.removeFromBlacklist (file); control->clearReason (file); }
    void allowAllAgain()                       { known.clearBlacklistedFiles(); control->clearReasons(); }

    /** Takes one instrument off the list (a rescan finds it again). */
    void forget (const juce::PluginDescription& d) { known.removeType (d); }

    /** "Rescan everything": forgets every plugin and every set-aside file. */
    void forgetEverything() { known.clear(); allowAllAgain(); }

    int timeoutSeconds() const { return control->timeoutMs.load() / 1000; }
    void setTimeoutSeconds (int s) { control->timeoutMs = juce::jlimit (5, 600, s) * 1000; }

    /** Instruments only, sorted by name -- what a column can hold. */
    juce::Array<juce::PluginDescription> instruments() const
    {
        juce::Array<juce::PluginDescription> out;
        for (const auto& d : known.getTypes())
            if (d.isInstrument) out.add (d);
        std::sort (out.begin(), out.end(), [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
                   { return a.name.compareIgnoreCase (b.name) < 0; });
        return out;
    }

    void loadFrom (juce::PropertiesFile& props)
    {
        if (auto xml = props.getXmlValue ("pluginList")) known.recreateFromXml (*xml);
        setTimeoutSeconds (props.getIntValue ("pluginScanTimeoutSeconds", 30));
        if (auto xml = props.getXmlValue ("pluginScanReasons"))
        {
            juce::StringPairArray reasons;
            for (auto* e : xml->getChildIterator())
                reasons.set (e->getStringAttribute ("file"), e->getStringAttribute ("why"));
            control->setAllReasons (reasons);
        }
    }

    void saveTo (juce::PropertiesFile& props)
    {
        if (auto xml = known.createXml()) props.setValue ("pluginList", xml.get());
        props.setValue ("pluginScanTimeoutSeconds", timeoutSeconds());
        juce::XmlElement reasonsXml ("REASONS");
        const auto reasons = control->allReasons();
        for (const auto& file : reasons.getAllKeys())
        {
            auto* e = reasonsXml.createNewChildElement ("FILE");
            e->setAttribute ("file", file);
            e->setAttribute ("why", reasons[file]);
        }
        props.setValue ("pluginScanReasons", &reasonsXml);
        props.saveIfNeeded();
    }

    /** Owner: "the app should reject other effect plugins -- only accepts
        instrument plugins". Effects stay in the scanned list, so a rescan
        doesn't check hundreds of them again, but instruments() and find()
        never offer one: nothing can load or save an effect. Effects on a
        column come from the built-in PERFORM LIVE channel strip instead.
        Returns how many effects the list holds. */
    int effectCount() const
    {
        int effects = 0;
        for (const auto& d : known.getTypes())
            if (! d.isInstrument) ++effects;
        return effects;
    }

    /** The directories a scan looks in: the format's own defaults (Program
        Files\Common Files\VST3 on Windows) -- the places installers put plugins. */
    juce::FileSearchPath defaultSearchPath() const
    {
        juce::FileSearchPath path;
        for (auto* f : formats.getFormats())
            path.addPath (f->getDefaultLocationsToSearch());
        return path;
    }

    std::optional<juce::PluginDescription> find (const juce::String& identifierString) const
    {
        for (const auto& d : known.getTypes())
            if (d.isInstrument && d.createIdentifierString() == identifierString) return d;
        return std::nullopt;
    }

private:
    std::shared_ptr<ScanControl> control { std::make_shared<ScanControl>() };   // before `known`, which hands it to the scanner
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
};

//==============================================================================
//  The song clock, as plugins want it.
//==============================================================================
class HostPlayHead final : public juce::AudioPlayHead
{
public:
    // written by the audio thread just before plugins render; read by them
    // inside processBlock on the same thread
    void update (bool playing, double bpm, int beatsPerBar, double ppq, int64_t samplePos)
    {
        juce::AudioPlayHead::PositionInfo p;
        p.setIsPlaying (playing);
        p.setBpm (bpm > 0.0 ? juce::Optional<double> (bpm) : juce::Optional<double>());
        p.setTimeSignature (juce::AudioPlayHead::TimeSignature { juce::jmax (1, beatsPerBar), 4 });
        p.setPpqPosition (ppq);
        p.setPpqPositionOfLastBarStart (std::floor (ppq / (double) juce::jmax (1, beatsPerBar)) * (double) juce::jmax (1, beatsPerBar));
        p.setTimeInSamples (samplePos);
        current = p;
    }

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override { return current; }

private:
    juce::AudioPlayHead::PositionInfo current;
};

//==============================================================================
//  One column's instrument.
//==============================================================================
class InstrumentSlot
{
public:
    static constexpr int kMaxBlock = 8192;
    static constexpr int kMaxChannels = 64;   // Kontakt has 34 (16 stereo outs + 2 ins); Vienna more

    /** Called from prepareToPlay (audio stopped). */
    void prepare (double sampleRate, int blockSize)
    {
        rate = sampleRate;
        block = juce::jmax (1, blockSize);
        // Sized ONCE here (audio stopped) for the widest plugin we will
        // host, so installing a plugin never resizes a buffer the audio
        // thread may be inside.
        scratch.setSize (kMaxChannels, juce::jmax (block, 64), false, true, true);
        if (auto* p = live.load (std::memory_order_acquire))
        {
            p->releaseResources();
            p->setRateAndBufferSizeDetails (rate, block);
            p->prepareToPlay (rate, block);
        }
        collector.reset (sampleRate);
    }

    /** Message thread: installs a freshly created instance (already prepared
        by `install`'s caller through prepare()). The previous one is parked
        until the audio thread confirms it is no longer using it. */
    void install (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::PluginDescription& desc)
    {
        if (instance != nullptr)
        {
            instance->enableAllBuses();
            instance->setPlayHead (playHead);
            instance->setRateAndBufferSizeDetails (rate, block);
            instance->prepareToPlay (rate, block);
            if (scratch.getNumSamples() < juce::jmax (block, 64)) scratch.setSize (kMaxChannels, juce::jmax (block, 64), false, true, true);
        }
        description = desc;
        auto* raw = instance.release();
        auto* old = live.exchange (raw, std::memory_order_acq_rel);
        if (old != nullptr) retired.emplace_back (old);
        collectRetired();
    }

    void clear()
    {
        description = {};
        auto* old = live.exchange (nullptr, std::memory_order_acq_rel);
        if (old != nullptr) retired.emplace_back (old);
        collectRetired();
    }

    /** Message thread, call from a timer. A replaced instance is stopped
        once the audio thread has let go of it -- and then kept, never
        deleted. Deleting the last instance of a plugin unloads its DLL, and
        plugin DLLs routinely crash on unload while their own threads are
        still running (every plugin tested did, in ntdll, at exit). Hosts
        keep modules loaded for the life of the process; so does this. A
        parked instance costs its memory only, and only when an instrument
        is swapped. */
    void collectRetired()
    {
        if (retired.empty()) return;
        if (audioSeen.load (std::memory_order_acquire) != live.load (std::memory_order_acquire)) return;
        for (auto& r : retired)
        {
            r->suspendProcessing (true);
            r->releaseResources();
            parkedForever().push_back (r.release());
        }
        retired.clear();
    }

    static std::vector<juce::AudioPluginInstance*>& parkedForever()
    {
        static auto* parked = new std::vector<juce::AudioPluginInstance*>();   // deliberately never freed
        return *parked;
    }

    void setPlayHead (juce::AudioPlayHead* ph)
    {
        playHead = ph;
        if (auto* p = live.load (std::memory_order_acquire)) p->setPlayHead (ph);
    }

    bool hasInstrument() const { return live.load (std::memory_order_relaxed) != nullptr; }
    const juce::PluginDescription& currentDescription() const { return description; }
    juce::AudioPluginInstance* instanceForEditor() { return live.load (std::memory_order_acquire); }   // message thread only

    /** MIDI from any thread (device callbacks) -- lock-free into the collector. */
    void addMidi (const juce::MidiMessage& m) { collector.addMessageToQueue (m); }
    void allNotesOff() { collector.addMessageToQueue (juce::MidiMessage::allNotesOff (1)); }

    /** Audio thread: OVERWRITES outL/outR with the instrument, or silence. */
    void render (float* outL, float* outR, int n)
    {
        auto* p = live.load (std::memory_order_acquire);
        audioSeen.store (p, std::memory_order_release);
        if (p == nullptr || n <= 0 || n > scratch.getNumSamples())
        {
            for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
            if (p == nullptr) { juce::MidiBuffer drop; collector.removeNextBlockOfMessages (drop, juce::jmax (1, n)); }
            return;
        }
        midi.clear();
        collector.removeNextBlockOfMessages (midi, n);
        scratch.clear();
        // The plugin gets a buffer with EVERY channel of every bus it has
        // (Kontakt: 16 stereo outputs), never just the two we listen to --
        // handing it fewer channels than its layout is how it writes past
        // the end and takes the app down inside its own runtime.
        const int chans = juce::jmax (2, p->getTotalNumInputChannels(), p->getTotalNumOutputChannels());
        if (chans > scratch.getNumChannels())
        {
            for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }   // wider than we host: silent, never unsafe
            return;
        }
        juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), chans, n);
        if (p->isSuspended()) view.clear();
        else                  p->processBlock (view, midi);
        const float* l = view.getReadPointer (0);
        const float* r = view.getNumChannels() > 1 ? view.getReadPointer (1) : l;
        for (int i = 0; i < n; ++i) { outL[i] = l[i]; outR[i] = r[i]; }
    }

    /** Plugin state for the project file (base64), and back. */
    juce::String stateAsString() const
    {
        auto* p = live.load (std::memory_order_acquire);
        if (p == nullptr) return {};
        juce::MemoryBlock mb;
        p->getStateInformation (mb);
        return mb.toBase64Encoding();
    }

    void setStateFromString (const juce::String& s)
    {
        auto* p = live.load (std::memory_order_acquire);
        if (p == nullptr || s.isEmpty()) return;
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (s) && mb.getSize() < 64 * 1024 * 1024)   // a state blob is data; still, cap it
            p->setStateInformation (mb.getData(), (int) mb.getSize());
    }

    ~InstrumentSlot()
    {
        // same rule at teardown: stop it, keep the module loaded
        if (auto* p = live.exchange (nullptr, std::memory_order_acq_rel)) { p->suspendProcessing (true); p->releaseResources(); parkedForever().push_back (p); }
        for (auto& r : retired) parkedForever().push_back (r.release());
        retired.clear();
    }

private:
    std::atomic<juce::AudioPluginInstance*> live { nullptr };
    std::atomic<juce::AudioPluginInstance*> audioSeen { nullptr };
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> retired;
    juce::PluginDescription description;
    juce::AudioPlayHead* playHead { nullptr };
    juce::MidiMessageCollector collector;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> scratch;
    double rate { 44100.0 };
    int block { 512 };
};

//==============================================================================
//  A window around a plugin's own editor.
//==============================================================================
class InstrumentEditorWindow final : public juce::DocumentWindow
{
public:
    InstrumentEditorWindow (juce::AudioPluginInstance& instance, const juce::String& title, std::function<void()> onClosed)
        : juce::DocumentWindow (title, juce::Colour (0xff0c0c17), juce::DocumentWindow::closeButton), onClose (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        if (auto* editor = instance.createEditorIfNeeded())
        {
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
        }
        else
        {
            auto* generic = new juce::GenericAudioProcessorEditor (instance);
            generic->setSize (480, 600);
            setContentOwned (generic, true);
            setResizable (true, false);
        }
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    // The owner deletes this window: after this call has returned, never while
    // the window is still running its own code.
    void closeButtonPressed() override
    {
        setVisible (false);
        if (onClose) juce::MessageManager::callAsync (onClose);
    }

private:
    std::function<void()> onClose;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentEditorWindow)
};

} // namespace ezinst
