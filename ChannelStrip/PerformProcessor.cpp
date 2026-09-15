#include "PerformProcessor.h"
#include "PerformEditor.h"

#include <cmath>
#include <complex>

namespace amanorsac::perform
{
namespace
{
constexpr double pi = juce::MathConstants<double>::pi;
constexpr double divisionBeats[] { 4.0, 2.0, 1.0, 1.5, 0.5, 0.75, 1.0 / 3.0, 0.25 };

void normalise(Biquad& f, double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
}

double clampFrequency(double rate, double f) noexcept { return juce::jlimit(10.0, rate * 0.49, f); }
}

void Biquad::setPeak(double r, double f, double q, double db) noexcept
{
    const auto A = std::pow(10.0, db / 40.0), w = 2.0 * pi * clampFrequency(r, f) / r;
    const auto alpha = std::sin(w) / (2.0 * q), cs = std::cos(w);
    normalise(*this, 1.0 + alpha * A, -2.0 * cs, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * cs, 1.0 - alpha / A);
}

void Biquad::setLowShelf(double r, double f, double db) noexcept
{
    const auto A = std::pow(10.0, db / 40.0), w = 2.0 * pi * clampFrequency(r, f) / r;
    const auto cs = std::cos(w), s = 2.0 * std::sqrt(A) * std::sin(w) / std::sqrt(2.0);
    normalise(*this, A * ((A + 1) - (A - 1) * cs + s), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - s),
              (A + 1) + (A - 1) * cs + s, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - s);
}

void Biquad::setHighShelf(double r, double f, double db) noexcept
{
    const auto A = std::pow(10.0, db / 40.0), w = 2.0 * pi * clampFrequency(r, f) / r;
    const auto cs = std::cos(w), s = 2.0 * std::sqrt(A) * std::sin(w) / std::sqrt(2.0);
    normalise(*this, A * ((A + 1) + (A - 1) * cs + s), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - s),
              (A + 1) - (A - 1) * cs + s, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - s);
}

void Biquad::setLowPass(double r, double f) noexcept
{
    const auto w = 2.0 * pi * clampFrequency(r, f) / r, cs = std::cos(w), alpha = std::sin(w) / (2.0 * 0.7071);
    normalise(*this, (1 - cs) * 0.5, 1 - cs, (1 - cs) * 0.5, 1 + alpha, -2 * cs, 1 - alpha);
}

double Biquad::magnitude(double r, double f) const noexcept
{
    const auto w = 2.0 * pi * f / r;
    const std::complex<double> e1 = std::polar(1.0, -w), e2 = std::polar(1.0, -2.0 * w);
    return std::abs((b0 + b1 * e1 + b2 * e2) / (1.0 + a1 * e1 + a2 * e2));
}

const juce::StringArray& reverbTypeNames()
{
    static const juce::StringArray names { "Room", "Hall", "Plate", "Chamber" };
    return names;
}

const juce::StringArray& delayDivisionNames()
{
    static const juce::StringArray names { "1/1", "1/2", "1/4", "1/4 D", "1/8", "1/8 D", "1/8 T", "1/16", "Free" };
    return names;
}

const juce::StringArray& presetCategoryOrder()
{
    static const juce::StringArray names { "General", "Vocals", "Speech", "Instruments", "Creative" };
    return names;
}

// ------------------------------------------------------------------ presets
const std::vector<FactoryPreset>& factoryPresets()
{
    // Values are in real units: dB, ms, %, s, Hz. Switches are 0/1 and menus
    // are the item index. Anything left out loads at its default.
    static const std::vector<FactoryPreset> bank = []
    {
        std::vector<FactoryPreset> b;
        auto add = [&b](const char* name, const char* category, std::vector<std::pair<const char*, float>> v)
        { b.push_back({ name, category, std::move(v) }); };

        // Loads when the plug-in opens, so it has to suit almost any source:
        // a touch of clean-up EQ, light levelling, a short room, no echo.
        // Every control is set here so it never depends on the defaults.
        add("Stage Ready", "General", { { "eq_on", 1 }, { "eq_low", -1 }, { "eq_lowmid", -1 }, { "eq_highmid", 1 }, { "eq_high", 1.5f },
            { "comp_on", 1 }, { "comp_threshold", -16 }, { "comp_ratio", 2.5f }, { "comp_attack", 15 }, { "comp_release", 150 }, { "comp_makeup", 2 },
            { "rev_on", 1 }, { "rev_type", 1 }, { "rev_mix", 12 }, { "rev_size", 45 }, { "rev_decay", 1.4f }, { "rev_tone", 50 }, { "rev_predelay", 15 },
            { "dly_on", 0 }, { "dly_div", 2 }, { "dly_mix", 15 }, { "dly_time", 500 }, { "dly_feedback", 30 }, { "dly_filter", 6000 }, { "dly_pingpong", 1 },
            { "out_gain", 0 }, { "out_mute", 0 } });

        add("Vocal Live", "Vocals", { { "eq_low", -2 }, { "eq_lowmid", -1.5f }, { "eq_highmid", 1.5f }, { "eq_high", 2.5f },
            { "comp_threshold", -18 }, { "comp_ratio", 4 }, { "comp_attack", 10 }, { "comp_release", 100 }, { "comp_makeup", 3 },
            { "rev_type", 1 }, { "rev_mix", 20 }, { "rev_size", 50 }, { "rev_decay", 2 }, { "rev_tone", 50 }, { "rev_predelay", 20 },
            { "dly_div", 2 }, { "dly_mix", 18 }, { "dly_feedback", 35 }, { "dly_filter", 8000 }, { "dly_pingpong", 1 } });
        add("Lead Vocal Upfront", "Vocals", { { "eq_low", -2 }, { "eq_lowmid", -3 }, { "eq_highmid", 2.5f }, { "eq_high", 3 },
            { "comp_threshold", -22 }, { "comp_ratio", 5 }, { "comp_attack", 5 }, { "comp_release", 80 }, { "comp_makeup", 5 },
            { "rev_type", 2 }, { "rev_mix", 14 }, { "rev_size", 45 }, { "rev_decay", 1.4f }, { "rev_tone", 60 }, { "rev_predelay", 30 },
            { "dly_div", 5 }, { "dly_mix", 10 }, { "dly_feedback", 25 }, { "dly_filter", 6000 }, { "dly_pingpong", 1 } });
        add("Soft Ballad", "Vocals", { { "eq_low", -2 }, { "eq_lowmid", 0 }, { "eq_highmid", 0 }, { "eq_high", 2 },
            { "comp_threshold", -20 }, { "comp_ratio", 3 }, { "comp_attack", 20 }, { "comp_release", 200 }, { "comp_makeup", 3 },
            { "rev_type", 1 }, { "rev_mix", 30 }, { "rev_size", 70 }, { "rev_decay", 3.5f }, { "rev_tone", 45 }, { "rev_predelay", 40 },
            { "dly_div", 2 }, { "dly_mix", 16 }, { "dly_feedback", 40 }, { "dly_filter", 5000 }, { "dly_pingpong", 1 } });
        add("Rap and Hype", "Vocals", { { "eq_low", -4 }, { "eq_lowmid", -2 }, { "eq_highmid", 3 }, { "eq_high", 1.5f },
            { "comp_threshold", -24 }, { "comp_ratio", 6 }, { "comp_attack", 3 }, { "comp_release", 60 }, { "comp_makeup", 6 },
            { "rev_type", 0 }, { "rev_mix", 8 }, { "rev_size", 30 }, { "rev_decay", 0.8f }, { "rev_tone", 55 }, { "rev_predelay", 5 },
            { "dly_div", 4 }, { "dly_mix", 12 }, { "dly_feedback", 20 }, { "dly_filter", 7000 }, { "dly_pingpong", 1 } });
        add("Slapback Rock", "Vocals", { { "eq_low", 0 }, { "eq_lowmid", -1 }, { "eq_highmid", 2 }, { "eq_high", 1.5f },
            { "comp_threshold", -20 }, { "comp_ratio", 5 }, { "comp_attack", 5 }, { "comp_release", 80 }, { "comp_makeup", 4 },
            { "rev_type", 0 }, { "rev_mix", 10 }, { "rev_size", 35 }, { "rev_decay", 0.9f }, { "rev_predelay", 0 },
            { "dly_div", 8 }, { "dly_time", 110 }, { "dly_mix", 22 }, { "dly_feedback", 8 }, { "dly_filter", 5000 }, { "dly_pingpong", 0 } });
        add("Worship Lead", "Vocals", { { "eq_low", -2 }, { "eq_lowmid", 0 }, { "eq_highmid", 1 }, { "eq_high", 2 },
            { "comp_threshold", -18 }, { "comp_ratio", 3 }, { "comp_attack", 15 }, { "comp_release", 150 }, { "comp_makeup", 3 },
            { "rev_type", 1 }, { "rev_mix", 32 }, { "rev_size", 85 }, { "rev_decay", 4.5f }, { "rev_tone", 40 }, { "rev_predelay", 35 },
            { "dly_div", 3 }, { "dly_mix", 20 }, { "dly_feedback", 45 }, { "dly_filter", 4500 }, { "dly_pingpong", 1 } });
        add("Gospel Power", "Vocals", { { "eq_low", 0 }, { "eq_lowmid", -2 }, { "eq_highmid", 2 }, { "eq_high", 2 },
            { "comp_threshold", -24 }, { "comp_ratio", 6 }, { "comp_attack", 8 }, { "comp_release", 120 }, { "comp_makeup", 6 },
            { "rev_type", 2 }, { "rev_mix", 22 }, { "rev_decay", 2.2f }, { "rev_tone", 55 }, { "rev_predelay", 25 },
            { "dly_div", 4 }, { "dly_mix", 12 }, { "dly_feedback", 30 }, { "dly_filter", 6000 } });
        add("Backing Vocals Wide", "Vocals", { { "eq_low", -5 }, { "eq_lowmid", -2 }, { "eq_highmid", -1 }, { "eq_high", 2 },
            { "comp_threshold", -20 }, { "comp_ratio", 4 }, { "comp_attack", 15 }, { "comp_release", 150 }, { "comp_makeup", 2 },
            { "rev_type", 1 }, { "rev_mix", 28 }, { "rev_size", 80 }, { "rev_decay", 2.5f }, { "rev_predelay", 10 },
            { "dly_div", 5 }, { "dly_mix", 8 }, { "dly_feedback", 30 }, { "dly_filter", 5000 }, { "dly_pingpong", 1 } });
        add("Choir Blend", "Vocals", { { "eq_low", -6 }, { "eq_lowmid", -2 }, { "eq_highmid", 0 }, { "eq_high", 1.5f },
            { "comp_threshold", -14 }, { "comp_ratio", 2.5f }, { "comp_attack", 30 }, { "comp_release", 250 }, { "comp_makeup", 2 },
            { "rev_type", 1 }, { "rev_mix", 35 }, { "rev_size", 90 }, { "rev_decay", 3.8f }, { "rev_tone", 45 }, { "rev_predelay", 15 },
            { "dly_on", 0 } });

        add("MC and Host", "Speech", { { "eq_low", -6 }, { "eq_lowmid", -2 }, { "eq_highmid", 3 }, { "eq_high", 1 },
            { "comp_threshold", -20 }, { "comp_ratio", 4 }, { "comp_attack", 5 }, { "comp_release", 120 }, { "comp_makeup", 5 },
            { "rev_on", 0 }, { "dly_on", 0 } });
        add("Preacher Room", "Speech", { { "eq_low", -5 }, { "eq_lowmid", -2.5f }, { "eq_highmid", 2 }, { "eq_high", 1.5f },
            { "comp_threshold", -22 }, { "comp_ratio", 3.5f }, { "comp_attack", 8 }, { "comp_release", 150 }, { "comp_makeup", 5 },
            { "rev_type", 0 }, { "rev_mix", 10 }, { "rev_size", 40 }, { "rev_decay", 1.1f }, { "rev_tone", 50 }, { "dly_on", 0 } });

        add("Acoustic Guitar Stage", "Instruments", { { "eq_low", -4 }, { "eq_lowmid", -3 }, { "eq_highmid", 1 }, { "eq_high", 3 },
            { "comp_threshold", -20 }, { "comp_ratio", 3 }, { "comp_attack", 20 }, { "comp_release", 150 }, { "comp_makeup", 2 },
            { "rev_type", 0 }, { "rev_mix", 15 }, { "rev_size", 45 }, { "rev_decay", 1.2f }, { "rev_tone", 55 }, { "dly_on", 0 } });
        add("Keys and Piano", "Instruments", { { "eq_low", -1 }, { "eq_lowmid", -2 }, { "eq_highmid", 0 }, { "eq_high", 1.5f },
            { "comp_threshold", -16 }, { "comp_ratio", 2.5f }, { "comp_attack", 25 }, { "comp_release", 200 }, { "comp_makeup", 1.5f },
            { "rev_type", 1 }, { "rev_mix", 18 }, { "rev_size", 60 }, { "rev_decay", 2 }, { "rev_predelay", 10 }, { "dly_on", 0 } });
        add("Lead Guitar Echo", "Instruments", { { "eq_low", -3 }, { "eq_lowmid", 0 }, { "eq_highmid", 1.5f }, { "eq_high", 0 },
            { "comp_threshold", -18 }, { "comp_ratio", 3 }, { "comp_attack", 15 }, { "comp_release", 150 }, { "comp_makeup", 2 },
            { "rev_type", 2 }, { "rev_mix", 15 }, { "rev_decay", 1.8f },
            { "dly_div", 5 }, { "dly_mix", 22 }, { "dly_feedback", 40 }, { "dly_filter", 4000 }, { "dly_pingpong", 1 } });
        add("Sax and Horns", "Instruments", { { "eq_low", -3 }, { "eq_lowmid", -1.5f }, { "eq_highmid", 1 }, { "eq_high", 1 },
            { "comp_threshold", -18 }, { "comp_ratio", 3 }, { "comp_attack", 10 }, { "comp_release", 120 }, { "comp_makeup", 2 },
            { "rev_type", 2 }, { "rev_mix", 18 }, { "rev_decay", 1.6f }, { "rev_predelay", 20 }, { "dly_on", 0 } });
        add("Bass DI Tight", "Instruments", { { "eq_low", 2 }, { "eq_lowmid", -2 }, { "eq_highmid", 1 }, { "eq_high", -3 },
            { "comp_threshold", -22 }, { "comp_ratio", 5 }, { "comp_attack", 15 }, { "comp_release", 150 }, { "comp_makeup", 4 },
            { "rev_on", 0 }, { "dly_on", 0 } });
        add("Drum Room", "Instruments", { { "eq_low", 2 }, { "eq_lowmid", -3 }, { "eq_highmid", 1.5f }, { "eq_high", 2 },
            { "comp_threshold", -20 }, { "comp_ratio", 4 }, { "comp_attack", 20 }, { "comp_release", 80 }, { "comp_makeup", 3 },
            { "rev_type", 0 }, { "rev_mix", 12 }, { "rev_size", 30 }, { "rev_decay", 0.7f }, { "rev_tone", 60 }, { "dly_on", 0 } });

        add("Ambient Swell", "Creative", { { "eq_low", -3 }, { "eq_lowmid", 0 }, { "eq_highmid", 0 }, { "eq_high", 1 },
            { "comp_threshold", -24 }, { "comp_ratio", 3 }, { "comp_attack", 40 }, { "comp_release", 300 }, { "comp_makeup", 3 },
            { "rev_type", 1 }, { "rev_mix", 55 }, { "rev_size", 100 }, { "rev_decay", 9 }, { "rev_tone", 35 }, { "rev_predelay", 60 },
            { "dly_div", 1 }, { "dly_mix", 40 }, { "dly_feedback", 65 }, { "dly_filter", 3000 }, { "dly_pingpong", 1 } });
        add("Dub Throw", "Creative", { { "eq_low", 0 }, { "eq_lowmid", 0 }, { "eq_highmid", 0 }, { "eq_high", 0 },
            { "comp_threshold", -18 }, { "comp_ratio", 4 }, { "comp_attack", 10 }, { "comp_release", 100 }, { "comp_makeup", 3 },
            { "rev_type", 2 }, { "rev_mix", 20 }, { "rev_decay", 2.5f },
            { "dly_div", 3 }, { "dly_mix", 45 }, { "dly_feedback", 75 }, { "dly_filter", 2500 }, { "dly_pingpong", 1 } });
        return b;
    }();
    return bank;
}

// ------------------------------------------------------------------ parameters
juce::AudioProcessorValueTreeState::ParameterLayout PerformProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto range = [](float lo, float hi, float centre, float step)
    {
        NormalisableRange<float> r(lo, hi, step);
        if (centre > lo && centre < hi) r.setSkewForCentre(centre);
        return r;
    };
    auto flt = [&layout](const char* id, const char* name, NormalisableRange<float> r, float def, const char* unit)
    { layout.add(std::make_unique<AudioParameterFloat>(ParameterID { id, 1 }, name, r, def, AudioParameterFloatAttributes().withLabel(unit))); };
    auto sw = [&layout](const char* id, const char* name, bool def)
    { layout.add(std::make_unique<AudioParameterBool>(ParameterID { id, 1 }, name, def)); };

    sw("eq_on", "EQ On", true);
    flt("eq_low", "EQ Low", range(-12, 12, 0, 0.1f), -2, "dB");
    flt("eq_lowmid", "EQ Low Mid", range(-12, 12, 0, 0.1f), -1.5f, "dB");
    flt("eq_highmid", "EQ High Mid", range(-12, 12, 0, 0.1f), 1.5f, "dB");
    flt("eq_high", "EQ High", range(-12, 12, 0, 0.1f), 2.5f, "dB");

    sw("comp_on", "Comp On", true);
    flt("comp_threshold", "Threshold", range(-48, 0, 0, 0.1f), -18, "dB");
    flt("comp_ratio", "Ratio", range(1, 20, 4, 0.1f), 4, ":1");
    flt("comp_attack", "Attack", range(0.1f, 100, 10, 0.1f), 10, "ms");
    flt("comp_release", "Release", range(10, 1000, 120, 1), 100, "ms");
    flt("comp_makeup", "Makeup", range(0, 24, 0, 0.1f), 3, "dB");

    sw("rev_on", "Reverb On", true);
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "rev_type", 1 }, "Reverb Type", reverbTypeNames(), 1));
    flt("rev_mix", "Reverb Mix", range(0, 100, 0, 1), 20, "%");
    flt("rev_size", "Reverb Size", range(0, 100, 0, 1), 50, "%");
    flt("rev_decay", "Reverb Decay", range(0.2f, 10, 2, 0.1f), 2, "s");
    flt("rev_tone", "Reverb Tone", range(0, 100, 0, 1), 50, "%");
    flt("rev_predelay", "Pre Delay", range(0, 200, 40, 1), 20, "ms");

    sw("dly_on", "Delay On", true);
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "dly_div", 1 }, "Delay Division", delayDivisionNames(), 2));
    flt("dly_mix", "Delay Mix", range(0, 100, 0, 1), 18, "%");
    flt("dly_time", "Delay Time", range(10, 2000, 300, 1), 500, "ms");
    flt("dly_feedback", "Feedback", range(0, 95, 0, 1), 35, "%");
    flt("dly_filter", "Delay Filter", range(1000, 20000, 5000, 10), 8000, "Hz");
    sw("dly_pingpong", "Ping Pong", true);

    flt("out_gain", "Output", range(-24, 12, 0, 0.1f), 0, "dB");
    sw("out_mute", "Mute", false);
    return layout;
}

PerformProcessor::PerformProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      state(*this, nullptr, "PERFORM_LIVE", createLayout())
{
    auto p = [this](const char* id) { return state.getRawParameterValue(id); };
    raw.eqOn = p("eq_on");
    for (int i = 0; i < 4; ++i) raw.eq[i] = p(eqIds[i]);
    raw.compOn = p("comp_on"); raw.threshold = p("comp_threshold"); raw.ratio = p("comp_ratio");
    raw.attack = p("comp_attack"); raw.release = p("comp_release"); raw.makeup = p("comp_makeup");
    raw.revOn = p("rev_on"); raw.revType = p("rev_type"); raw.revMix = p("rev_mix"); raw.revSize = p("rev_size");
    raw.revDecay = p("rev_decay"); raw.revTone = p("rev_tone"); raw.revPre = p("rev_predelay");
    raw.dlyOn = p("dly_on"); raw.dlyDiv = p("dly_div"); raw.dlyMix = p("dly_mix"); raw.dlyTime = p("dly_time");
    raw.dlyFeedback = p("dly_feedback"); raw.dlyFilter = p("dly_filter"); raw.dlyPing = p("dly_pingpong");
    raw.outGain = p("out_gain"); raw.mute = p("out_mute");
    loadFactory(0);
}

bool PerformProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet() == out;
}

void PerformProcessor::prepareToPlay(double sampleRate, int)
{
    rate = sampleRate;
    // Nothing in the path looks ahead or oversamples: the dry signal leaves
    // in the same sample it arrived.
    setLatencySamples(0);

    for (auto& f : eq) f.reset();
    for (auto& line : echo) line.prepare(static_cast<int>(rate * 4.2) + 8);
    for (auto& line : preDelay) line.prepare(static_cast<int>(rate * 0.25) + 8);
    reverb.setSampleRate(rate);
    reverb.reset();
    revTone.reset();
    echoFilter.reset();
    envelope = 0.0f;
    for (auto& v : eqApplied) v = 99.0f;
    revToneApplied = echoFilterApplied = -1.0f;
    reverbApplied = {};
    reverbApplied.roomSize = -1.0f;

    for (auto& g : eqGain) g.reset(rate, 0.05);
    for (auto* s : { &eqEnable, &compEnable, &makeup, &revSend, &revWet, &revMix, &dlySend, &dlyWet, &dlyMix, &feedback, &outGain, &muteGain })
        s->reset(rate, 0.03);
    preDelaySamples.reset(rate, 0.1);
    echoSamples.reset(rate, 0.2);

    updateBlockTargets();
    for (auto& g : eqGain) g.setCurrentAndTargetValue(g.getTargetValue());
    for (auto* s : { &eqEnable, &compEnable, &makeup, &revSend, &revWet, &revMix, &dlySend, &dlyWet, &dlyMix, &feedback,
                     &outGain, &muteGain, &preDelaySamples, &echoSamples })
        s->setCurrentAndTargetValue(s->getTargetValue());
}

void PerformProcessor::updateBlockTargets()
{
    auto on = [](const std::atomic<float>* v) { return v->load() > 0.5f ? 1.0f : 0.0f; };

    for (int i = 0; i < 4; ++i) eqGain[i].setTargetValue(raw.eq[i]->load());
    eqEnable.setTargetValue(on(raw.eqOn));

    compEnable.setTargetValue(on(raw.compOn));
    threshold = raw.threshold->load();
    ratio = juce::jmax(1.0f, raw.ratio->load());
    makeup.setTargetValue(raw.makeup->load());
    attackCoef = static_cast<float>(std::exp(-1.0 / (juce::jmax(0.05f, raw.attack->load()) * 0.001 * rate)));
    releaseCoef = static_cast<float>(std::exp(-1.0 / (juce::jmax(1.0f, raw.release->load()) * 0.001 * rate)));

    const auto revOn = on(raw.revOn);
    revSend.setTargetValue(revOn);
    revWet.setTargetValue(revOn);
    revMix.setTargetValue(raw.revMix->load() * 0.01f);
    preDelaySamples.setTargetValue(static_cast<float>(juce::jmax(1.0, raw.revPre->load() * 0.001 * rate)));

    struct Voice { float scale, damping, width; };
    static constexpr Voice voices[] { { 0.55f, 0.50f, 0.70f }, { 0.86f, 0.35f, 1.0f }, { 0.78f, 0.15f, 1.0f }, { 0.70f, 0.45f, 0.9f } };
    const auto& voice = voices[juce::jlimit(0, 3, static_cast<int>(raw.revType->load()))];
    const auto size = raw.revSize->load() * 0.01f, tone = raw.revTone->load() * 0.01f;
    const auto decayNorm = static_cast<float>(std::log(raw.revDecay->load() / 0.2) / std::log(50.0));
    juce::Reverb::Parameters params;
    params.roomSize = juce::jlimit(0.05f, 0.98f, voice.scale * (0.35f + 0.65f * decayNorm) + (size - 0.5f) * 0.12f);
    params.damping = juce::jlimit(0.0f, 1.0f, voice.damping + (0.5f - tone) * 0.6f);
    params.width = voice.width * (0.4f + 0.6f * size);
    params.wetLevel = 0.33f;
    params.dryLevel = 0.0f;
    params.freezeMode = 0.0f;
    if (params.roomSize != reverbApplied.roomSize || params.damping != reverbApplied.damping || params.width != reverbApplied.width)
    {
        reverb.setParameters(params);
        reverbApplied = params;
    }
    if (tone != revToneApplied) { revTone.setLowPass(rate, 2000.0 * std::pow(8.0, static_cast<double>(tone))); revToneApplied = tone; }

    const auto dlyOn = on(raw.dlyOn);
    dlySend.setTargetValue(dlyOn);
    dlyWet.setTargetValue(dlyOn);
    dlyMix.setTargetValue(raw.dlyMix->load() * 0.01f);
    feedback.setTargetValue(raw.dlyFeedback->load() * 0.01f);
    pingPong = raw.dlyPing->load() > 0.5f;

    double bpm = 120.0;
    if (auto* head = getPlayHead())
        if (auto position = head->getPosition())
            if (auto tempo = position->getBpm()) bpm = juce::jlimit(20.0, 400.0, *tempo);
    const auto division = juce::jlimit(0, freeDivision, static_cast<int>(raw.dlyDiv->load()));
    const auto seconds = division == freeDivision ? raw.dlyTime->load() * 0.001 : divisionBeats[division] * 60.0 / bpm;
    echoSamples.setTargetValue(static_cast<float>(juce::jlimit(1.0, rate * 4.0, seconds * rate)));

    const auto filter = raw.dlyFilter->load();
    if (filter != echoFilterApplied) { echoFilter.setLowPass(rate, filter); echoFilterApplied = filter; }

    outGain.setTargetValue(juce::Decibels::decibelsToGain(raw.outGain->load()));
    muteGain.setTargetValue(raw.mute->load() > 0.5f ? 0.0f : 1.0f);
}

void PerformProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto samples = buffer.getNumSamples();
    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, samples);
    const auto channels = juce::jmin(2, buffer.getNumChannels());
    if (channels == 0 || samples == 0) return;

    updateBlockTargets();
    auto* left = buffer.getWritePointer(0);
    auto* right = channels > 1 ? buffer.getWritePointer(1) : nullptr;
    const auto ping = pingPong && right != nullptr;
    const auto knee = 6.0f, slope = 1.0f - 1.0f / ratio;
    float blockReduction = 0.0f, blockPeak = 0.0f;

    for (int start = 0; start < samples; start += chunk)
    {
        const auto count = juce::jmin(chunk, samples - start);

        // EQ coefficients follow the smoothed gains a chunk at a time.
        for (int b = 0; b < 4; ++b)
        {
            const auto gain = eqGain[b].skip(count);
            if (std::abs(gain - eqApplied[b]) > 0.001f)
            {
                if (b == 0)      eq[0].setLowShelf(rate, eqFrequencies[0], gain);
                else if (b == 3) eq[3].setHighShelf(rate, eqFrequencies[3], gain);
                else             eq[static_cast<size_t>(b)].setPeak(rate, eqFrequencies[b], 0.9, gain);
                eqApplied[b] = gain;
            }
        }

        for (int i = 0; i < count; ++i)
        {
            const auto k = static_cast<size_t>(i);
            auto l = left[start + i];
            auto r = right != nullptr ? right[start + i] : l;

            // EQ runs all the time so switching it in never clicks.
            auto el = l, er = r;
            for (auto& band : eq) { el = band.process(el, 0); er = right != nullptr ? band.process(er, 1) : el; }
            const auto eqW = eqEnable.getNextValue();
            l += eqW * (el - l);
            r += eqW * (er - r);

            // Compressor: stereo-linked peak detector with a 6 dB soft knee.
            const auto levelDb = juce::Decibels::gainToDecibels(juce::jmax(std::abs(l), std::abs(r)), -120.0f);
            const auto over = levelDb - threshold;
            const auto target = over <= -knee * 0.5f ? 0.0f
                              : over >= knee * 0.5f  ? over * slope
                                                     : slope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
            envelope = target > envelope ? attackCoef * envelope + (1.0f - attackCoef) * target
                                         : releaseCoef * envelope + (1.0f - releaseCoef) * target;
            const auto compW = compEnable.getNextValue();
            const auto compGain = 1.0f + compW * (juce::Decibels::decibelsToGain(makeup.getNextValue() - envelope) - 1.0f);
            l *= compGain;
            r *= compGain;
            blockReduction = juce::jmax(blockReduction, envelope * compW);
            dryL[k] = l;
            dryR[k] = r;

            // Reverb send through the pre-delay. Only the wet path is delayed.
            const auto send = revSend.getNextValue();
            preDelay[0].push(l * send);
            preDelay[1].push(r * send);
            const auto pre = preDelaySamples.getNextValue();
            revL[k] = preDelay[0].read(pre);
            revR[k] = preDelay[1].read(pre);
            revGate[k] = revWet.getNextValue() * revMix.getNextValue();

            // Delay, with the filter inside the feedback loop.
            const auto time = echoSamples.getNextValue();
            const auto dl = echoFilter.process(echo[0].read(time), 0);
            const auto dr = echoFilter.process(echo[1].read(time), 1);
            const auto fb = feedback.getNextValue();
            const auto in = dlySend.getNextValue();
            if (ping)
            {
                echo[0].push(0.5f * (l + r) * in + fb * dr);
                echo[1].push(fb * dl);
            }
            else
            {
                echo[0].push(l * in + fb * dl);
                echo[1].push(r * in + fb * dr);
            }
            echoL[k] = dl;
            echoR[k] = dr;
            echoGate[k] = dlyWet.getNextValue() * dlyMix.getNextValue();
        }

        reverb.processStereo(revL.data(), revR.data(), count);

        for (int i = 0; i < count; ++i)
        {
            const auto k = static_cast<size_t>(i);
            const auto wl = revTone.process(revL[k], 0), wr = revTone.process(revR[k], 1);
            const auto gain = outGain.getNextValue() * muteGain.getNextValue();
            const auto ol = (dryL[k] + revGate[k] * wl + echoGate[k] * echoL[k]) * gain;
            const auto orr = (dryR[k] + revGate[k] * wr + echoGate[k] * echoR[k]) * gain;
            if (right != nullptr) { left[start + i] = ol; right[start + i] = orr; }
            else left[start + i] = 0.5f * (ol + orr);
            blockPeak = juce::jmax(blockPeak, std::abs(ol), std::abs(orr));
        }
    }

    gainReductionDb.store(blockReduction);
    outputPeak.store(juce::jmax(outputPeak.load(), blockPeak));
}

// ------------------------------------------------------------------ presets and state
void PerformProcessor::applyValues(const std::vector<std::pair<const char*, float>>& values)
{
    for (auto* parameter : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter))
            ranged->setValueNotifyingHost(ranged->getDefaultValue());
    for (const auto& [id, value] : values)
        if (auto* parameter = state.getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        else
            jassertfalse;   // a preset names a control that does not exist
}

void PerformProcessor::loadFactory(int index)
{
    const auto& bank = factoryPresets();
    if (! juce::isPositiveAndBelow(index, static_cast<int>(bank.size()))) return;
    currentFactory = index;
    currentName = bank[static_cast<size_t>(index)].name;
    applyValues(bank[static_cast<size_t>(index)].values);
}

void PerformProcessor::stepPreset(int delta)
{
    const auto count = static_cast<int>(factoryPresets().size());
    loadFactory(((juce::jmax(0, currentFactory) + delta) % count + count) % count);
}

const juce::String PerformProcessor::getProgramName(int index)
{
    const auto& bank = factoryPresets();
    return juce::isPositiveAndBelow(index, static_cast<int>(bank.size())) ? bank[static_cast<size_t>(index)].name : juce::String();
}

juce::File PerformProcessor::userPresetFolder()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("Amanorsac Studio").getChildFile("Presets").getChildFile("PERFORM LIVE");
}

juce::Array<juce::File> PerformProcessor::userPresets()
{
    auto files = userPresetFolder().findChildFiles(juce::File::findFiles, false, "*.amanorsacpreset");
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileName().compareIgnoreCase(b.getFileName()) < 0; });
    return files;
}

bool PerformProcessor::saveUserPreset(const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName(name.trim());
    if (clean.isEmpty()) return false;
    const auto folder = userPresetFolder();
    if (! folder.createDirectory()) return false;
    const auto xml = state.copyState().createXml();
    if (xml == nullptr || ! xml->writeTo(folder.getChildFile(clean + ".amanorsacpreset"))) return false;
    currentName = clean;
    currentFactory = -1;
    return true;
}

bool PerformProcessor::loadUserPreset(const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr || ! xml->hasTagName(state.state.getType())) return false;
    state.replaceState(juce::ValueTree::fromXml(*xml));
    currentName = file.getFileNameWithoutExtension();
    currentFactory = -1;
    return true;
}

void PerformProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    auto tree = state.copyState();
    tree.setProperty("preset", currentName, nullptr);
    tree.setProperty("factory", currentFactory, nullptr);
    if (const auto xml = tree.createXml()) copyXmlToBinary(*xml, destination);
}

void PerformProcessor::setStateInformation(const void* data, int size)
{
    const auto xml = getXmlFromBinary(data, size);
    if (xml == nullptr || ! xml->hasTagName(state.state.getType())) return;
    const auto tree = juce::ValueTree::fromXml(*xml);
    currentName = tree.getProperty("preset", currentName).toString();
    currentFactory = static_cast<int>(tree.getProperty("factory", -1));
    state.replaceState(tree);
}

juce::AudioProcessorEditor* PerformProcessor::createEditor() { return new PerformEditor(*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new amanorsac::perform::PerformProcessor(); }
