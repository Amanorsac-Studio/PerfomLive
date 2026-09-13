// ============================================================================
//  MidiActionRouter.h — learnable MIDI CC/note bindings (Milestone 14,
//  PRODUCT_REQUIREMENTS.md §15). JUCE-side (juce::MidiInputCallback).
//
//  Thread safety, the one genuinely new hazard this milestone introduces
//  (project/MILESTONE_11-15_ARCHITECTURE.md's resolved MIDI ownership
//  decision): juce::MidiInputCallback::handleIncomingMidiMessage() fires on
//  a MIDI driver thread -- a THIRD thread this codebase has never had to
//  reason about (previously: audio thread + message thread only). Every
//  incoming message is marshaled to the message thread via
//  juce::MessageManager::callAsync() BEFORE any binding lookup or action
//  invocation -- preserving the "at most one non-audio writer thread"
//  invariant every prior milestone's thread-safety argument depends on,
//  rather than quietly introducing a new concurrent writer.
//
//  A learned CC can be bound either as a continuous fader (0-127 mapped to
//  0.0-1.0, e.g. a mixer channel gain) or as a button-style action (values
//  >63 register as a press, ignoring the release half -- PRODUCT_REQUIREMENTS.md
//  §15's own stated rule, `fireBinding` in the JS reference). Never both for
//  the same CC number.
// ============================================================================
#pragma once
#include <JuceHeader.h>
#include "ActionRegistry.h"
#include <map>
#include <optional>

namespace ezaction
{

class MidiActionRouter : public juce::MidiInputCallback
{
public:
    explicit MidiActionRouter (ActionRegistry& registryToUse) : registry (registryToUse) {}

    void bindCcButton (int cc, ActionId action) { ccFaderBindings.erase (cc); ccButtonBindings[cc] = action; }
    void bindCcFader (int cc, std::function<void (float)> onValue) { ccButtonBindings.erase (cc); ccFaderBindings[cc] = std::move (onValue); }
    void bindNote (int note, ActionId action) { noteBindings[note] = action; }
    void unbindCc (int cc)     { ccButtonBindings.erase (cc); ccFaderBindings.erase (cc); }
    void unbindNote (int note) { noteBindings.erase (note); }

    // Learn mode: arms a target; the NEXT incoming CC/note-on binds to it,
    // then learn mode clears itself. Message-thread-only (armed from the
    // Settings panel's MIDI tab).
    void armLearnButton (ActionId action) { learnButtonTarget = action; learnFaderTarget = nullptr; }
    void armLearnFader (std::function<void (float)> onValue) { learnFaderTarget = std::move (onValue); learnButtonTarget.reset(); }
    void cancelLearn() { learnButtonTarget.reset(); learnFaderTarget = nullptr; }
    bool isLearning() const { return learnButtonTarget.has_value() || (bool) learnFaderTarget; }

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message) override
    {
        const auto msg = message;   // copy -- safe to hop threads with
        juce::MessageManager::callAsync ([this, msg] { processOnMessageThread (msg); });
    }

    void handlePartialSysexMessage (juce::MidiInput*, const juce::uint8*, int, double) override {}

    const std::map<int, ActionId>& ccButtons() const { return ccButtonBindings; }
    const std::map<int, ActionId>& notes() const     { return noteBindings; }

private:
    void processOnMessageThread (const juce::MidiMessage& msg)
    {
        if (msg.isController())
        {
            const int cc    = msg.getControllerNumber();
            const int value = msg.getControllerValue();

            if (learnFaderTarget)
            {
                ccFaderBindings[cc] = std::move (learnFaderTarget);
                learnFaderTarget = nullptr;
                return;
            }
            if (learnButtonTarget.has_value())
            {
                ccButtonBindings[cc] = *learnButtonTarget;
                learnButtonTarget.reset();
                return;
            }

            if (auto it = ccFaderBindings.find (cc); it != ccFaderBindings.end())
            {
                if (it->second) it->second ((float) value / 127.0f);
                return;
            }
            if (auto it = ccButtonBindings.find (cc); it != ccButtonBindings.end())
            {
                // PRD §15: a CC learned onto a button-style action treats
                // values >63 as a press, ignoring the release half.
                if (value > 63) registry.invoke (it->second);
            }
        }
        else if (msg.isNoteOn())
        {
            const int note = msg.getNoteNumber();
            if (learnButtonTarget.has_value())
            {
                noteBindings[note] = *learnButtonTarget;
                learnButtonTarget.reset();
                return;
            }
            if (auto it = noteBindings.find (note); it != noteBindings.end())
                registry.invoke (it->second);
        }
    }

    ActionRegistry& registry;
    std::map<int, ActionId> ccButtonBindings;
    std::map<int, std::function<void (float)>> ccFaderBindings;
    std::map<int, ActionId> noteBindings;

    std::optional<ActionId> learnButtonTarget;
    std::function<void (float)> learnFaderTarget;
};

} // namespace ezaction
