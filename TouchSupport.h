// ============================================================================
//  TouchSupport.h -- press-and-hold as the touch equivalent of right-click.
//
//  Owner: "consider making the app iPad friendly". A finger has no right
//  button, so every menu that only opened on right-click was unreachable on
//  an iPad or a touchscreen laptop. A component owns one LongPress, feeds it
//  its mouse events, and gets the same menu after a short hold. The hold is
//  cancelled by movement, so dragging a fader or a section boundary never
//  turns into a menu, and a completed hold swallows the tap that would
//  otherwise follow it.
// ============================================================================
#pragma once

#include <JuceHeader.h>

#include <functional>

namespace eztouch
{

class LongPress : private juce::Timer
{
public:
    static constexpr int kHoldMs = 450;         // long enough not to fire on a tap, short enough to feel direct
    static constexpr int kSlopPixels = 10;      // a finger wobbles; more than this is a drag

    std::function<void (juce::Point<int> screenPos)> onLongPress;

    /** From mouseDown. Right-click needs no hold, so it is not started. */
    void begin (const juce::MouseEvent& e)
    {
        fired = false;
        if (e.mods.isPopupMenu()) { stopTimer(); return; }
        startScreen = e.getScreenPosition();
        startTimer (kHoldMs);
    }

    /** From mouseDrag: movement cancels the hold. */
    void drag (const juce::MouseEvent& e)
    {
        if (isTimerRunning() && e.getScreenPosition().getDistanceFrom (startScreen) > kSlopPixels)
            stopTimer();
    }

    /** From mouseUp. True when the press already became a long-press, so the
        caller must not also treat it as a tap. */
    bool end()
    {
        stopTimer();
        const bool was = fired;
        fired = false;
        return was;
    }

    bool hasFired() const { return fired; }
    void cancel() { stopTimer(); fired = false; }

private:
    void timerCallback() override
    {
        stopTimer();
        fired = true;
        if (onLongPress) onLongPress (startScreen);
    }

    juce::Point<int> startScreen;
    bool fired { false };
};

} // namespace eztouch
