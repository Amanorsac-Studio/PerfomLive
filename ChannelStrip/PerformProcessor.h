#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <vector>

namespace amanorsac::perform
{
/** RBJ biquad in transposed direct form II. Coefficients are plain doubles so
    they can change on the audio thread without allocating. */
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1[2] {}, z2[2] {};

    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    float process(float x, int channel) noexcept
    {
        const double y = b0 * x + z1[channel];
        z1[channel] = b1 * x - a1 * y + z2[channel];
        z2[channel] = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
    void setPeak(double rate, double frequency, double q, double gainDb) noexcept;
    void setLowShelf(double rate, double frequency, double gainDb) noexcept;
    void setHighShelf(double rate, double frequency, double gainDb) noexcept;
    void setLowPass(double rate, double frequency) noexcept;
    [[nodiscard]] double magnitude(double rate, double frequency) const noexcept;
};

/** Fractional circular delay, sized once in prepare. */
struct DelayLine
{
    std::vector<float> data;
    int write = 0;

    void prepare(int length) { data.assign(static_cast<size_t>(juce::jmax(4, length)), 0.0f); write = 0; }
    void push(float x) noexcept
    {
        data[static_cast<size_t>(write)] = x;
        if (++write == static_cast<int>(data.size())) write = 0;
    }
    /** A delay of 1 returns the sample pushed last. */
    [[nodiscard]] float read(double delay) const noexcept
    {
        const auto size = static_cast<int>(data.size());
        delay = juce::jlimit(1.0, static_cast<double>(size - 2), delay);
        auto position = static_cast<double>(write) - delay;
        if (position < 0.0) position += size;
        const auto i0 = static_cast<int>(position);
        const auto i1 = i0 + 1 == size ? 0 : i0 + 1;
        const auto frac = static_cast<float>(position - i0);
        return data[static_cast<size_t>(i0)] + frac * (data[static_cast<size_t>(i1)] - data[static_cast<size_t>(i0)]);
    }
};

struct FactoryPreset
{
    juce::String name, category;
    std::vector<std::pair<const char*, float>> values;
};

const std::vector<FactoryPreset>& factoryPresets();
const juce::StringArray& presetCategoryOrder();
const juce::StringArray& reverbTypeNames();
const juce::StringArray& delayDivisionNames();
inline constexpr int freeDivision = 8;

inline constexpr double eqFrequencies[4] { 100.0, 400.0, 3000.0, 10000.0 };
inline constexpr const char* eqIds[4] { "eq_low", "eq_lowmid", "eq_highmid", "eq_high" };

class PerformProcessor final : public juce::AudioProcessor
{
public:
    PerformProcessor();
    ~PerformProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "PERFORM LIVE"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }

    int getNumPrograms() override { return static_cast<int>(factoryPresets().size()); }
    int getCurrentProgram() override { return juce::jmax(0, currentFactory); }
    void setCurrentProgram(int index) override { loadFactory(index); }
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // presets -------------------------------------------------------------
    void loadFactory(int index);
    void stepPreset(int delta);
    [[nodiscard]] juce::String presetName() const { return currentName; }
    static juce::File userPresetFolder();
    [[nodiscard]] static juce::Array<juce::File> userPresets();
    bool saveUserPreset(const juce::String& name);
    bool loadUserPreset(const juce::File& file);

    // meters, read by the editor ------------------------------------------
    std::atomic<float> gainReductionDb { 0.0f };
    std::atomic<float> outputPeak { 0.0f };

    juce::AudioProcessorValueTreeState state;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void applyValues(const std::vector<std::pair<const char*, float>>& values);
    void updateBlockTargets();

    struct Raw
    {
        std::atomic<float>* eqOn = nullptr;
        std::atomic<float>* eq[4] {};
        std::atomic<float>* compOn = nullptr, *threshold = nullptr, *ratio = nullptr, *attack = nullptr,
                          *release = nullptr, *makeup = nullptr;
        std::atomic<float>* revOn = nullptr, *revType = nullptr, *revMix = nullptr, *revSize = nullptr,
                          *revDecay = nullptr, *revTone = nullptr, *revPre = nullptr;
        std::atomic<float>* dlyOn = nullptr, *dlyDiv = nullptr, *dlyMix = nullptr, *dlyTime = nullptr,
                          *dlyFeedback = nullptr, *dlyFilter = nullptr, *dlyPing = nullptr;
        std::atomic<float>* outGain = nullptr, *mute = nullptr;
    } raw;

    double rate = 48000.0;
    static constexpr int chunk = 32;

    std::array<Biquad, 4> eq;
    juce::SmoothedValue<float> eqGain[4];
    float eqApplied[4] { 99.0f, 99.0f, 99.0f, 99.0f };
    juce::SmoothedValue<float> eqEnable, compEnable, makeup;
    float envelope = 0.0f, attackCoef = 0.0f, releaseCoef = 0.0f, threshold = -18.0f, ratio = 4.0f;

    juce::Reverb reverb;
    juce::Reverb::Parameters reverbApplied;
    DelayLine preDelay[2];
    juce::SmoothedValue<float> preDelaySamples, revSend, revWet, revMix;
    Biquad revTone;
    float revToneApplied = -1.0f;

    DelayLine echo[2];
    juce::SmoothedValue<float> echoSamples, dlySend, dlyWet, dlyMix, feedback;
    Biquad echoFilter;
    float echoFilterApplied = -1.0f;
    bool pingPong = true;

    juce::SmoothedValue<float> outGain, muteGain;

    std::array<float, chunk> dryL {}, dryR {}, revL {}, revR {}, echoL {}, echoR {}, revGate {}, echoGate {};

    int currentFactory = 0;
    juce::String currentName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformProcessor)
};
}
