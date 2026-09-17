// ============================================================================
//  KeyBindingMap.h — rebindable keyboard shortcuts (Milestone 14,
//  PRODUCT_REQUIREMENTS.md §15). JUCE-side (juce::KeyPress/juce::String).
//
//  resetToDefaults() reproduces PRD §15's own default keymap table exactly.
//  Keyed by juce::KeyPress::getTextDescription() rather than juce::KeyPress
//  itself (which has no operator< suitable for std::map) -- a reasonable,
//  documented simplification since every distinct keypress produces a
//  distinct description string in practice.
// ============================================================================
#pragma once
#include <JuceHeader.h>
#include "ActionRegistry.h"
#include <map>

namespace ezaction
{

class KeyBindingMap
{
public:
    KeyBindingMap() { resetToDefaults(); }

    // PRODUCT_REQUIREMENTS.md §15's DEFAULT_KEYS table, reproduced exactly.
    // Scenes are learnable only -- no keyboard default, per that same table.
    void resetToDefaults()
    {
        bindings.clear();
        bind (juce::KeyPress (juce::KeyPress::spaceKey), ActionId::PlayStop);
        bind (juce::KeyPress (juce::KeyPress::returnKey), ActionId::PlayNext);
        bind (juce::KeyPress (juce::KeyPress::downKey), ActionId::NextDeck);
        bind (juce::KeyPress (juce::KeyPress::upKey), ActionId::PrevDeck);
        bind (juce::KeyPress ((int) 'T'), ActionId::TapTempo);
        bind (juce::KeyPress ((int) 'B'), ActionId::ToggleMetronome);
        bind (juce::KeyPress ((int) 'G'), ActionId::ToggleTempoLock);
        bind (juce::KeyPress (juce::KeyPress::backspaceKey), ActionId::AllPadsOff);

        // Section playback. Chosen to collide with nothing above and nothing
        // in the pad/FX letter rows: arrows for sections, Page keys for
        // songs, End/Escape/Home for the jump/cancel/loop trio a performer
        // reaches for mid-song, F5 for a counted-in start, F2 for the view.
        bind (juce::KeyPress (juce::KeyPress::rightKey),    ActionId::NextSection);
        bind (juce::KeyPress (juce::KeyPress::leftKey),     ActionId::PrevSection);
        bind (juce::KeyPress (juce::KeyPress::pageDownKey), ActionId::NextSong);
        bind (juce::KeyPress (juce::KeyPress::pageUpKey),   ActionId::PrevSong);
        bind (juce::KeyPress (juce::KeyPress::endKey),      ActionId::JumpNow);
        bind (juce::KeyPress (juce::KeyPress::escapeKey),   ActionId::CancelJump);
        bind (juce::KeyPress (juce::KeyPress::homeKey),     ActionId::LoopSection);
        bind (juce::KeyPress (juce::KeyPress::F5Key),       ActionId::CountInPlay);
        bind (juce::KeyPress (juce::KeyPress::F2Key),       ActionId::TogglePlaybackView);

        // Live tempo: the +/- keys (= is + without Shift), and the number pad's.
        bind (juce::KeyPress ((int) '='),                          ActionId::TempoUp);
        bind (juce::KeyPress ((int) '-'),                          ActionId::TempoDown);
        bind (juce::KeyPress (juce::KeyPress::numberPadAdd),       ActionId::TempoUp);
        bind (juce::KeyPress (juce::KeyPress::numberPadSubtract),  ActionId::TempoDown);

        for (int i = 0; i < 8; ++i)
            bind (juce::KeyPress ((int) ('1' + i)), deckSlotAction (i));

        static const char padKeys[12] = { 'Q','W','E','R','A','S','D','F','Z','X','C','V' };
        for (int i = 0; i < 12; ++i)
            bind (juce::KeyPress ((int) padKeys[i]), padAction (i));

        static const char fxKeys[12] = { 'Y','U','I','O','H','J','K','L','N','M',',','.' };
        for (int i = 0; i < 12; ++i)
            bind (juce::KeyPress ((int) fxKeys[i]), fxAction (i));
    }

    void bind (const juce::KeyPress& key, ActionId action) { bindings[keyToString (key)] = action; }
    void unbind (const juce::KeyPress& key)                { bindings.erase (keyToString (key)); }

    bool actionFor (const juce::KeyPress& key, ActionId& outAction) const
    {
        auto it = bindings.find (keyToString (key));
        if (it == bindings.end()) return false;
        outAction = it->second;
        return true;
    }

    // For UI display and persistence.
    const std::map<juce::String, ActionId>& all() const           { return bindings; }
    void setAll (const std::map<juce::String, ActionId>& newMap)  { bindings = newMap; }

    static juce::String keyToString (const juce::KeyPress& key) { return key.getTextDescription(); }

private:
    std::map<juce::String, ActionId> bindings;
};

} // namespace ezaction
