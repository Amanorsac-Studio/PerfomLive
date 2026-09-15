#pragma once

#include "PerformProcessor.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace amanorsac::perform
{
/** The PERFORM LIVE window. Everything is laid out on a fixed 1536 x 1024
    canvas and scaled as one piece, so the proportions never drift. */
class PerformEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit PerformEditor(PerformProcessor&);
    ~PerformEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    class Surface;
    std::unique_ptr<Surface> surface;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformEditor)
};
}
