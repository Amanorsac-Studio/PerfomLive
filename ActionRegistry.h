// ============================================================================
//  ActionRegistry.h — the single source of truth for "what does this action
//  do," shared by mouse clicks, keyboard shortcuts, and MIDI (Milestone 14,
//  PRODUCT_REQUIREMENTS.md §15; project/MILESTONE_11-15_ARCHITECTURE.md's
//  resolved "Action registry architecture / command routing").
//
//  Pure C++, no JUCE -- a plain ActionId enum and a callback table. Keyboard
//  (KeyBindingMap) and MIDI (MidiActionRouter) are separate, JUCE-side input
//  layers that both resolve down to an ActionId and invoke through this SAME
//  registry -- never three separate trigger implementations for one action
//  (ENGINEERING_PRINCIPLES.md's "one path for a state change").
//
//  Pad/FX/Scene/Deck-slot actions are contiguous ranges rather than 40
//  hand-written enumerators -- padAction(i)/fxAction(i)/sceneAction(i)/
//  deckSlotAction(i) compute the right ActionId, matching
//  PRODUCT_REQUIREMENTS.md §15's own default keymap table exactly (Decks
//  A1-A4/B1-B4, Pads 1-12, FX 1-12, Scenes 1-8).
// ============================================================================
#pragma once
#include <array>
#include <functional>

namespace ezaction
{

enum class ActionId : int
{
    PlayStop = 0, PlayNext, NextDeck, PrevDeck, TapTempo, ToggleMetronome, ToggleTempoLock, AllPadsOff,
    DeckSlotBase,                                  // + 0..7  (A1-A4, B1-B4 -- PRD §15)
    PadBase   = DeckSlotBase + 8,                  // + 0..11
    FxBase    = PadBase + 12,                      // + 0..11
    SceneBase = FxBase + 12,                       // + 0..7

    // Section playback (AbleSet-style). Appended AFTER the ranges above so
    // every existing ActionId keeps its integer -- saved keyboard/MIDI
    // bindings identify actions by that integer, and renumbering would
    // silently remap a performer's footswitches.
    NextSection = SceneBase + 8, PrevSection, NextSong, PrevSong,
    JumpNow, CancelJump, LoopSection, CountInPlay, TogglePlaybackView,

    // Live tempo: 1 BPM up or down, at the next bar while playing. Appended
    // for the same reason as the section actions above.
    TempoUp, TempoDown,

    kCount
};

constexpr int kNumActions = (int) ActionId::kCount;

inline ActionId deckSlotAction (int slot) { return (ActionId) ((int) ActionId::DeckSlotBase + slot); }
inline ActionId padAction      (int idx)  { return (ActionId) ((int) ActionId::PadBase + idx); }
inline ActionId fxAction       (int idx)  { return (ActionId) ((int) ActionId::FxBase + idx); }
inline ActionId sceneAction    (int idx)  { return (ActionId) ((int) ActionId::SceneBase + idx); }

class ActionRegistry
{
public:
    void setAction (ActionId id, std::function<void()> fn) { actions[(size_t) id] = std::move (fn); }

    // Safe no-op if nothing (or an out-of-range id) is registered.
    void invoke (ActionId id) const
    {
        const size_t i = (size_t) id;
        if (i < actions.size() && actions[i]) actions[i]();
    }

    bool hasAction (ActionId id) const
    {
        const size_t i = (size_t) id;
        return i < actions.size() && (bool) actions[i];
    }

private:
    std::array<std::function<void()>, (size_t) kNumActions> actions;
};

} // namespace ezaction
