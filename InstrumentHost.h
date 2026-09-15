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

    void handleConnectionLost() override { juce::JUCEApplicationBase::quit(); }

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
//  The HOST side of the scan: launches the subprocess and asks it about one
//  file at a time. If the subprocess dies, that file is skipped and a fresh
//  subprocess handles the next one.
//==============================================================================
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        if (addPluginDescriptions (format.getName(), fileOrIdentifier, result))
            return true;
        worker = nullptr;   // it crashed on this file: the next file gets a new one
        return false;
    }

    void scanFinished() override { worker = nullptr; }

private:
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

    bool addPluginDescriptions (const juce::String& formatName, const juce::String& fileOrIdentifier,
                                juce::OwnedArray<juce::PluginDescription>& result)
    {
        if (worker == nullptr) worker = std::make_unique<Worker>();

        juce::MemoryBlock block;
        juce::MemoryOutputStream stream { block, true };
        stream.writeString (formatName);
        stream.writeString (fileOrIdentifier);
        if (! worker->sendMessageToWorker (block)) return false;

        // a plugin that hangs its own probe is abandoned after this long
        const auto deadline = juce::Time::getMillisecondCounter() + 60'000;
        for (;;)
        {
            if (shouldExit()) return true;
            if (juce::Time::getMillisecondCounter() > deadline) return false;
            const auto response = worker->getResponse();
            if (response.state == Worker::State::timeout) continue;
            if (response.xml != nullptr)
                for (const auto* item : response.xml->getChildIterator())
                {
                    auto desc = std::make_unique<juce::PluginDescription>();
                    if (desc->loadFromXml (*item)) result.add (std::move (desc));
                }
            return response.state == Worker::State::gotResult;
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
        known.setCustomScanner (std::make_unique<OutOfProcessScanner>());
    }

    juce::AudioPluginFormatManager& formatManager() { return formats; }
    juce::KnownPluginList& list() { return known; }

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
        keepInstrumentsOnly();
    }

    void saveTo (juce::PropertiesFile& props)
    {
        keepInstrumentsOnly();
        if (auto xml = known.createXml()) props.setValue ("pluginList", xml.get());
        props.saveIfNeeded();
    }

    /** Owner: "the app should reject other effect plugins -- only accepts
        instrument plugins". A scan finds everything in the VST3 folder; this
        drops every effect from the list, so an effect can never be offered,
        loaded or saved. Effects on a column come from the built-in PERFORM
        LIVE channel strip instead. Returns how many were dropped. */
    int keepInstrumentsOnly()
    {
        int dropped = 0;
        for (const auto& d : known.getTypes())
            if (! d.isInstrument) { known.removeType (d); ++dropped; }
        return dropped;
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

    void closeButtonPressed() override { if (onClose) onClose(); }

private:
    std::function<void()> onClose;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentEditorWindow)
};

} // namespace ezinst
