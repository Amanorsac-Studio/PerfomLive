#include "PerformEditor.h"

#include <cmath>

namespace amanorsac::perform
{
namespace
{
constexpr float canvasWidth = 1536.0f, canvasHeight = 1024.0f;

const juce::Colour textMain { 0xffe9edf2 }, textDim { 0xff8e97a3 };
const juce::Colour blue { 0xff2f8bff }, green { 0xff2fd36b }, purple { 0xffa65cf2 }, orange { 0xffff7a1a };

juce::Font uiFont(float size, bool bold = false, float kerning = 0.0f)
{
   #if JUCE_WINDOWS
    const juce::String name = "Segoe UI";
   #elif JUCE_MAC
    const juce::String name = "Helvetica Neue";
   #else
    const auto name = juce::Font::getDefaultSansSerifFontName();
   #endif
    return juce::Font(juce::FontOptions(name, size, bold ? juce::Font::bold : juce::Font::plain)).withExtraKerningFactor(kerning);
}

float textWidth(const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(font, text, 0.0f, 0.0f);
    return glyphs.getBoundingBox(0, -1, true).getWidth();
}

juce::String trimmed(float value, int decimals)
{
    const auto rounded = std::round(value);
    return std::abs(value - rounded) < 0.05f ? juce::String(static_cast<int>(rounded)) : juce::String(value, decimals);
}

// ------------------------------------------------------------------ look
class Look final : public juce::LookAndFeel_V4
{
public:
    Look()
    {
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff161a21));
        setColour(juce::PopupMenu::textColourId, textMain);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xff2a3242));
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour(juce::ComboBox::textColourId, textMain);
        setColour(juce::AlertWindow::backgroundColourId, juce::Colour(0xff161a21));
        setColour(juce::AlertWindow::textColourId, textMain);
        setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0d1015));
        setColour(juce::TextEditor::textColourId, textMain);
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232a35));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float position,
                          float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto accent = juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(
            slider.getProperties().getWithDefault("accent", static_cast<juce::int64>(0xffffffff)))));
        const auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                                   static_cast<float>(width), static_cast<float>(height));
        const auto centre = bounds.getCentre();
        const auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f;
        const auto thickness = radius * 0.1f;
        const auto arcRadius = radius - thickness * 0.5f;
        const auto angle = startAngle + position * (endAngle - startAngle);
        const juce::PathStrokeType stroke(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        juce::Path track;
        track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour(juce::Colour(0xff1c2029));
        g.strokePath(track, stroke);

        if (position > 0.001f)
        {
            juce::Path value;
            value.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
            g.setColour(accent);
            g.strokePath(value, stroke);
        }

        const auto knob = radius * 0.8f;
        const auto face = juce::Rectangle<float>(knob * 2.0f, knob * 2.0f).withCentre(centre);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.fillEllipse(face.translated(0.0f, knob * 0.08f).expanded(knob * 0.05f));
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff363a42), centre.x, face.getY(),
                                               juce::Colour(0xff0e1014), centre.x, face.getBottom(), false));
        g.fillEllipse(face);
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        g.drawEllipse(face.reduced(0.8f), 1.2f);

        const juce::Point<float> direction(std::sin(angle), -std::cos(angle));
        juce::Path pointer;
        pointer.startNewSubPath(centre + direction * (knob * 0.36f));
        pointer.lineTo(centre + direction * (knob * 0.78f));
        g.setColour(juce::Colours::white.withAlpha(0.96f));
        g.strokePath(pointer, juce::PathStrokeType(juce::jmax(2.5f, knob * 0.075f), juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox&) override
    {
        const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.6f);
        g.setColour(juce::Colour(0xff171b22));
        g.fillRoundedRectangle(r, 7.0f);
        g.setColour(juce::Colour(0xff2b313b));
        g.drawRoundedRectangle(r, 7.0f, 1.2f);

        const auto cx = static_cast<float>(width) - 34.0f, cy = static_cast<float>(height) * 0.5f;
        juce::Path chevron;
        chevron.startNewSubPath(cx - 7.0f, cy - 3.5f);
        chevron.lineTo(cx, cy + 3.5f);
        chevron.lineTo(cx + 7.0f, cy - 3.5f);
        g.setColour(textMain);
        g.strokePath(chevron, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return uiFont(24.0f); }
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(12, 0, box.getWidth() - 60, box.getHeight());
        label.setFont(getComboBoxFont(box));
    }
    juce::Font getPopupMenuFont() override { return uiFont(17.0f); }
};

// ------------------------------------------------------------------ buttons
class PowerButton final : public juce::ToggleButton
{
public:
    explicit PowerButton(juce::Colour colour) : accent(colour) {}

    void paintButton(juce::Graphics& g, bool over, bool) override
    {
        const auto r = getLocalBounds().toFloat().reduced(2.0f);
        const auto on = getToggleState();
        const auto base = on ? accent : juce::Colour(0xff3a404a);

        g.setColour(base.withAlpha(on ? 0.3f : 0.15f));
        g.fillEllipse(r);
        g.setGradientFill(juce::ColourGradient(base.brighter(0.3f), r.getCentreX(), r.getY(),
                                               base.darker(0.3f), r.getCentreX(), r.getBottom(), false));
        g.fillEllipse(r.reduced(3.0f));
        if (over)
        {
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.fillEllipse(r.reduced(3.0f));
        }

        const auto c = r.getCentre();
        const auto glyph = r.getWidth() * 0.22f;
        juce::Path power;
        power.addCentredArc(c.x, c.y, glyph, glyph, 0.0f, juce::MathConstants<float>::pi * 0.22f,
                            juce::MathConstants<float>::pi * 1.78f, true);
        power.startNewSubPath(c.x, c.y - glyph * 1.25f);
        power.lineTo(c.x, c.y - glyph * 0.15f);
        g.setColour(on ? juce::Colours::white : textDim);
        g.strokePath(power, juce::PathStrokeType(r.getWidth() * 0.07f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

private:
    juce::Colour accent;
};

class PillToggle final : public juce::ToggleButton
{
public:
    void paintButton(juce::Graphics& g, bool, bool) override
    {
        const auto r = getLocalBounds().toFloat().reduced(1.0f);
        const auto on = getToggleState();
        const auto radius = r.getHeight() * 0.5f;
        g.setColour(on ? orange : juce::Colour(0xff2b313b));
        g.fillRoundedRectangle(r, radius);
        const auto dot = juce::Rectangle<float>(r.getHeight() - 10.0f, r.getHeight() - 10.0f)
                             .withCentre({ on ? r.getRight() - radius : r.getX() + radius, r.getCentreY() });
        g.setColour(on ? juce::Colours::white : textDim);
        g.fillEllipse(dot);
    }
};

class MuteButton final : public juce::ToggleButton
{
public:
    void paintButton(juce::Graphics& g, bool over, bool) override
    {
        const auto r = getLocalBounds().toFloat().reduced(1.0f);
        const auto on = getToggleState();
        g.setColour(on ? juce::Colour(0xffd83b3b) : juce::Colour(over ? 0xff1b2029 : 0xff131720));
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(on ? juce::Colour(0xffff6b6b) : juce::Colour(0xff2f3540));
        g.drawRoundedRectangle(r, 8.0f, 1.2f);
        g.setColour(on ? juce::Colours::white : textMain);
        g.setFont(uiFont(20.0f));
        g.drawText("MUTE", r, juce::Justification::centred, false);
    }
};

class ChevronButton final : public juce::Button
{
public:
    explicit ChevronButton(bool pointsRight) : juce::Button({}), right(pointsRight) {}

    void paintButton(juce::Graphics& g, bool over, bool) override
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        juce::Path chevron;
        const auto d = right ? 1.0f : -1.0f;
        chevron.startNewSubPath(c.x - 4.0f * d, c.y - 9.0f);
        chevron.lineTo(c.x + 4.0f * d, c.y);
        chevron.lineTo(c.x - 4.0f * d, c.y + 9.0f);
        g.setColour(over ? juce::Colours::white : textMain);
        g.strokePath(chevron, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    bool right;
};

class DotsButton final : public juce::Button
{
public:
    DotsButton() : juce::Button({}) {}
    void paintButton(juce::Graphics& g, bool over, bool) override
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        g.setColour(over ? juce::Colours::white : textMain);
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({ c.x, c.y + static_cast<float>(i) * 12.0f }));
    }
};

struct ClickArea final : public juce::Component
{
    std::function<void()> onClick;
    void mouseUp(const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
};

// ------------------------------------------------------------------ meters and graph
float meterY(float db)
{
    static constexpr float dbs[] { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -60.0f };
    static constexpr float ys[] { 235.0f, 295.0f, 357.0f, 428.0f, 508.0f, 583.0f };
    if (db >= 0.0f) return 235.0f - db * 10.0f;
    for (int i = 1; i < 6; ++i)
        if (db >= dbs[i])
            return juce::jmap(db, dbs[i - 1], dbs[i], ys[i - 1], ys[i]);
    return 583.0f + (-60.0f - db) * 0.5f;
}

float meterDb(float y)
{
    static constexpr float dbs[] { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -60.0f };
    static constexpr float ys[] { 235.0f, 295.0f, 357.0f, 428.0f, 508.0f, 583.0f };
    if (y <= 235.0f) return (235.0f - y) / 10.0f;
    for (int i = 1; i < 6; ++i)
        if (y <= ys[i])
            return juce::jmap(y, ys[i - 1], ys[i], dbs[i - 1], dbs[i]);
    return -60.0f - (y - 583.0f) * 2.0f;
}

class EqGraph final : public juce::Component
{
public:
    explicit EqGraph(PerformProcessor& owner) : processor(owner) {}

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff0b0e13));
        g.fillRoundedRectangle(r, 6.0f);

        g.setColour(juce::Colour(0xff1b2029));
        for (const auto f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
            g.drawVerticalLine(juce::roundToInt(xFor(f)), r.getY(), r.getBottom());
        for (int db = -12; db <= 12; db += 6)
            g.drawHorizontalLine(juce::roundToInt(yFor(static_cast<float>(db))), r.getX(), r.getRight());
        g.setColour(juce::Colour(0xff2a303a));
        g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);

        const auto bands = currentBands();
        juce::Path curve;
        for (int px = 0; px <= getWidth(); px += 2)
        {
            const auto f = frequencyFor(static_cast<float>(px));
            double gain = 1.0;
            for (const auto& band : bands) gain *= band.magnitude(displayRate, f);
            const auto y = yFor(static_cast<float>(juce::Decibels::gainToDecibels(gain)));
            if (px == 0) curve.startNewSubPath(0.0f, y); else curve.lineTo(static_cast<float>(px), y);
        }

        juce::ColourGradient spectrum(blue, 0.0f, 0.0f, juce::Colour(0xffef4b4b), r.getRight(), 0.0f, false);
        spectrum.addColour(0.38, green);
        spectrum.addColour(0.68, juce::Colour(0xfff3c832));

        auto fill = curve;
        fill.lineTo(r.getRight(), r.getBottom());
        fill.lineTo(0.0f, r.getBottom());
        fill.closeSubPath();
        auto fade = spectrum;
        fade.multiplyOpacity(0.28f);
        g.setGradientFill(fade);
        g.fillPath(fill);
        g.setGradientFill(spectrum);
        g.strokePath(curve, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        static const juce::Colour nodeColours[] { blue, green, juce::Colour(0xfff3c832), juce::Colour(0xffef4b4b) };
        for (int i = 0; i < 4; ++i)
        {
            const auto p = nodePosition(i);
            g.setColour(juce::Colours::white);
            g.fillEllipse(juce::Rectangle<float>(20.0f, 20.0f).withCentre(p));
            g.setColour(nodeColours[i]);
            g.fillEllipse(juce::Rectangle<float>(15.0f, 15.0f).withCentre(p));
        }

        if (processor.state.getRawParameterValue("eq_on")->load() < 0.5f)
        {
            g.setColour(juce::Colour(0xff0b0e13).withAlpha(0.55f));
            g.fillRoundedRectangle(r, 6.0f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        auto best = 0;
        for (int i = 1; i < 4; ++i)
            if (nodePosition(i).getDistanceFrom(e.position) < nodePosition(best).getDistanceFrom(e.position)) best = i;
        dragging = processor.state.getParameter(eqIds[best]);
        if (dragging != nullptr) dragging->beginChangeGesture();
        mouseDrag(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging == nullptr) return;
        const auto db = juce::jlimit(-12.0f, 12.0f, juce::jmap(e.position.y, 0.0f, static_cast<float>(getHeight()), 15.0f, -15.0f));
        dragging->setValueNotifyingHost(dragging->convertTo0to1(db));
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging != nullptr) dragging->endChangeGesture();
        dragging = nullptr;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        for (int i = 0; i < 4; ++i)
            if (nodePosition(i).getDistanceFrom(e.position) < 16.0f)
                if (auto* p = processor.state.getParameter(eqIds[i]))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost(p->convertTo0to1(0.0f));
                    p->endChangeGesture();
                }
        repaint();
    }

private:
    static constexpr double displayRate = 48000.0;

    [[nodiscard]] std::array<Biquad, 4> currentBands() const
    {
        std::array<Biquad, 4> bands;
        for (int i = 0; i < 4; ++i)
        {
            const auto gain = processor.state.getRawParameterValue(eqIds[i])->load();
            if (i == 0)      bands[0].setLowShelf(displayRate, eqFrequencies[0], gain);
            else if (i == 3) bands[3].setHighShelf(displayRate, eqFrequencies[3], gain);
            else             bands[static_cast<size_t>(i)].setPeak(displayRate, eqFrequencies[i], 0.9, gain);
        }
        return bands;
    }

    [[nodiscard]] float xFor(double frequency) const
    {
        return static_cast<float>(std::log(frequency / 20.0) / std::log(1000.0)) * static_cast<float>(getWidth());
    }
    [[nodiscard]] double frequencyFor(float x) const
    {
        return 20.0 * std::pow(1000.0, static_cast<double>(x) / juce::jmax(1, getWidth()));
    }
    [[nodiscard]] float yFor(float db) const
    {
        return juce::jmap(juce::jlimit(-15.0f, 15.0f, db), 15.0f, -15.0f, 6.0f, static_cast<float>(getHeight()) - 6.0f);
    }
    [[nodiscard]] juce::Point<float> nodePosition(int index) const
    {
        return { xFor(eqFrequencies[index]), yFor(processor.state.getRawParameterValue(eqIds[index])->load()) };
    }

    PerformProcessor& processor;
    juce::RangedAudioParameter* dragging = nullptr;
};
}

// ------------------------------------------------------------------ surface
class PerformEditor::Surface final : public juce::Component
{
public:
    explicit Surface(PerformProcessor& owner)
        : processor(owner), graph(owner),
          power { PowerButton(blue), PowerButton(green), PowerButton(purple), PowerButton(orange) },
          prev(false), next(true)
    {
        setLookAndFeel(&look);
        setSize(static_cast<int>(canvasWidth), static_cast<int>(canvasHeight));

        static const char* powerIds[] { "eq_on", "comp_on", "rev_on", "dly_on" };
        for (int i = 0; i < 4; ++i)
        {
            addAndMakeVisible(power[i]);
            power[i].setBounds(juce::Rectangle<int>(52, 52).withCentre({ juce::roundToInt(panels[i].getX()) + 48, 161 }));
            buttonAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
                processor.state, powerIds[i], power[i]));
            power[i].onStateChange = [this] { refreshModules(); };
        }

        addAndMakeVisible(graph);

        addKnob("eq_low", "LOW", 0, blue, { 118, 588 });
        addKnob("eq_lowmid", "LOW MID", 0, green, { 290, 588 });
        addKnob("eq_highmid", "HIGH MID", 0, juce::Colour(0xfff3c832), { 118, 775 });
        addKnob("eq_high", "HIGH", 0, juce::Colour(0xffef4b4b), { 290, 775 });

        addKnob("comp_threshold", "THRESHOLD", 1, green, { 560, 395 }, true);
        addKnob("comp_ratio", "RATIO", 1, green, { 482, 592 });
        addKnob("comp_attack", "ATTACK", 1, green, { 643, 592 });
        addKnob("comp_release", "RELEASE", 1, green, { 482, 786 });
        addKnob("comp_makeup", "MAKEUP", 1, green, { 643, 786 });

        addKnob("rev_mix", "MIX", 2, purple, { 894, 390 }, true);
        addKnob("rev_size", "SIZE", 2, purple, { 817, 592 });
        addKnob("rev_decay", "DECAY", 2, purple, { 972, 592 });
        addKnob("rev_tone", "TONE", 2, purple, { 817, 782 });
        addKnob("rev_predelay", "PRE DELAY", 2, purple, { 972, 782 });

        addKnob("dly_mix", "MIX", 3, orange, { 1223, 390 }, true);
        addKnob("dly_time", "TIME", 3, orange, { 1147, 592 });
        addKnob("dly_feedback", "FEEDBACK", 3, orange, { 1300, 592 });
        addKnob("dly_filter", "FILTER", 3, orange, { 1147, 782 });

        addKnob("out_gain", "", 4, juce::Colours::white, { 1458, 693 });

        addCombo(reverbType, "rev_type", reverbTypeNames(), { 769, 221, 257, 51 });
        addCombo(delayDivision, "dly_div", delayDivisionNames(), { 1100, 221, 255, 51 });
        delayDivision.onChange = [this] { refreshModules(); repaint(); };

        addAndMakeVisible(pingPong);
        buttonAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.state, "dly_pingpong", pingPong));
        pingPong.setBounds(1260, 762, 84, 40);

        addAndMakeVisible(mute);
        buttonAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.state, "out_mute", mute));
        mute.setBounds(1407, 812, 102, 52);

        graph.setBounds(40, 225, 331, 215);

        addAndMakeVisible(prev);
        addAndMakeVisible(next);
        addAndMakeVisible(presetName);
        addAndMakeVisible(dots);
        prev.setBounds(1090, 26, 50, 54);
        next.setBounds(1400, 26, 50, 54);
        presetName.setBounds(1140, 26, 260, 54);
        dots.setBounds(1470, 28, 36, 50);
        prev.onClick = [this] { processor.stepPreset(-1); presetChanged(); };
        next.onClick = [this] { processor.stepPreset(1); presetChanged(); };
        presetName.onClick = [this] { showPresetMenu(); };
        dots.onClick = [this] { showOptionsMenu(); };

        shownPreset = processor.presetName();
        refreshModules();
    }

    ~Surface() override { setLookAndFeel(nullptr); }

    void tick()
    {
        const auto reduction = processor.gainReductionDb.load();
        const auto peak = processor.outputPeak.exchange(0.0f);
        const auto peakDb = juce::Decibels::gainToDecibels(peak, -90.0f);
        meterLevel = juce::jmax(peakDb, meterLevel - 1.2f);
        if (std::abs(reduction - shownReduction) > 0.05f) { shownReduction = reduction; repaint(427, 250, 272, 28); }
        repaint(1426, 198, 38, 418);

        if (processor.presetName() != shownPreset) presetChanged();
        graph.repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0a0c10));
        drawHeader(g);

        static const char* titles[] { "EQ", "COMP", "REVERB", "DELAY" };
        static const juce::Colour accents[] { blue, green, purple, orange };
        for (int i = 0; i < 4; ++i) drawModule(g, panels[i], accents[i], titles[i], power[i].getToggleState());

        drawCompMeter(g);
        drawEqScale(g);
        drawOutput(g);
        drawKnobText(g);

        g.setColour(textDim);
        g.setFont(uiFont(19.0f, true));
        g.drawText("PING PONG", juce::Rectangle<float>(1226, 726, 150, 24), juce::Justification::centred, false);

        drawFooter(g);
    }

private:
    struct Knob
    {
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
        juce::String id, label;
        int module = 0;
        juce::Point<float> centre;
        float size = 108.0f;
    };

    void addKnob(const char* id, const char* label, int module, juce::Colour accent, juce::Point<float> centre, bool large = false)
    {
        Knob knob;
        knob.id = id;
        knob.label = label;
        knob.module = module;
        knob.centre = centre;
        knob.size = large ? 140.0f : 108.0f;
        knob.slider = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        knob.slider->getProperties().set("accent", static_cast<juce::int64>(accent.getARGB()));
        knob.slider->setRotaryParameters(juce::MathConstants<float>::pi * 1.22f, juce::MathConstants<float>::pi * 2.78f, true);
        knob.slider->setMouseDragSensitivity(260);
        knob.slider->setBounds(juce::Rectangle<float>(knob.size, knob.size).withCentre(centre).toNearestInt());
        addAndMakeVisible(*knob.slider);
        knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.state, id, *knob.slider);
        if (auto* parameter = processor.state.getParameter(id))
            knob.slider->setDoubleClickReturnValue(true, parameter->convertFrom0to1(parameter->getDefaultValue()));
        knob.slider->onValueChange = [this] { repaint(); };
        knobs.push_back(std::move(knob));
    }

    void addCombo(juce::ComboBox& box, const char* id, const juce::StringArray& items, juce::Rectangle<int> bounds)
    {
        box.addItemList(items, 1);
        box.setBounds(bounds);
        addAndMakeVisible(box);
        comboAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.state, id, box));
    }

    void refreshModules()
    {
        for (auto& knob : knobs)
            if (knob.module < 4) knob.slider->setAlpha(power[knob.module].getToggleState() ? 1.0f : 0.4f);
        reverbType.setAlpha(power[2].getToggleState() ? 1.0f : 0.4f);
        delayDivision.setAlpha(power[3].getToggleState() ? 1.0f : 0.4f);
        pingPong.setAlpha(power[3].getToggleState() ? 1.0f : 0.4f);
        graph.repaint();
        repaint();
    }

    void presetChanged()
    {
        shownPreset = processor.presetName();
        repaint();
    }

    // ------------------------------------------------------------------ menus
    void showPresetMenu()
    {
        juce::PopupMenu menu;
        const auto& bank = factoryPresets();
        for (const auto& category : presetCategoryOrder())
        {
            juce::PopupMenu sub;
            for (int i = 0; i < static_cast<int>(bank.size()); ++i)
                if (bank[static_cast<size_t>(i)].category == category)
                    sub.addItem(i + 1, bank[static_cast<size_t>(i)].name, true, bank[static_cast<size_t>(i)].name == shownPreset);
            menu.addSubMenu(category, sub);
        }
        const auto user = PerformProcessor::userPresets();
        if (! user.isEmpty())
        {
            juce::PopupMenu mine;
            for (int i = 0; i < user.size(); ++i)
                mine.addItem(1000 + i, user[i].getFileNameWithoutExtension(), true, user[i].getFileNameWithoutExtension() == shownPreset);
            menu.addSeparator();
            menu.addSubMenu("My Presets", mine);
        }

        juce::Component::SafePointer<Surface> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetName), [safe, user](int result)
        {
            if (safe == nullptr || result <= 0) return;
            if (result < 1000) safe->processor.loadFactory(result - 1);
            else safe->processor.loadUserPreset(user[result - 1000]);
            safe->presetChanged();
        });
    }

    void showOptionsMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Save preset...");
        menu.addItem(2, "Open presets folder");
        juce::Component::SafePointer<Surface> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&dots), [safe](int result)
        {
            if (safe == nullptr) return;
            if (result == 1) safe->askForPresetName();
            if (result == 2)
            {
                auto folder = PerformProcessor::userPresetFolder();
                folder.createDirectory();
                folder.startAsProcess();
            }
        });
    }

    void askForPresetName()
    {
        auto* window = new juce::AlertWindow("Save preset", "Name this preset.", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", shownPreset);
        window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<Surface> safe(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create([safe, window](int result)
        {
            if (safe != nullptr && result == 1)
            {
                safe->processor.saveUserPreset(window->getTextEditorContents("name"));
                safe->presetChanged();
            }
        }), true);
    }

    // ------------------------------------------------------------------ drawing
    void drawHeader(juce::Graphics& g)
    {
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff151a22), 0.0f, 0.0f, juce::Colour(0xff0c0f14), 0.0f, 100.0f, false));
        g.fillRect(0.0f, 0.0f, canvasWidth, 100.0f);
        g.setColour(juce::Colour(0xff1c2129));
        g.drawHorizontalLine(100, 0.0f, canvasWidth);

        const auto titleFont = uiFont(38.0f, true, 0.06f);
        g.setFont(titleFont);
        g.setColour(juce::Colours::white);
        g.drawText("PERFORM", juce::Rectangle<float>(50.0f, 20.0f, 300.0f, 46.0f), juce::Justification::centredLeft, false);
        const auto performWidth = textWidth(titleFont, "PERFORM ");
        g.setColour(blue);
        g.drawText("LIVE", juce::Rectangle<float>(50.0f + performWidth, 20.0f, 200.0f, 46.0f), juce::Justification::centredLeft, false);

        g.setColour(textDim);
        g.setFont(uiFont(14.0f, false, 0.42f));
        g.drawText("AMANORSAC.STUDIO", juce::Rectangle<float>(52.0f, 62.0f, 300.0f, 20.0f), juce::Justification::centredLeft, false);

        g.setColour(juce::Colour(0xffa8b0bc));
        g.setFont(uiFont(26.0f, false, 0.55f));
        g.drawText("CHANNEL STRIP", juce::Rectangle<float>(518.0f, 30.0f, 500.0f, 46.0f), juce::Justification::centred, false);

        const auto bar = juce::Rectangle<float>(1090.0f, 26.0f, 360.0f, 54.0f);
        g.setColour(juce::Colour(0xff151a21));
        g.fillRoundedRectangle(bar, 9.0f);
        g.setColour(juce::Colour(0xff2b313b));
        g.drawRoundedRectangle(bar.reduced(0.6f), 9.0f, 1.2f);
        g.drawVerticalLine(1140, 27.0f, 79.0f);
        g.drawVerticalLine(1400, 27.0f, 79.0f);
        g.setColour(textMain);
        g.setFont(uiFont(24.0f));
        g.drawFittedText(shownPreset, juce::Rectangle<int>(1146, 26, 248, 54), juce::Justification::centred, 1, 0.8f);
    }

    void drawModule(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent, const juce::String& title, bool on)
    {
        g.setColour(juce::Colour(0xff10131a));
        g.fillRoundedRectangle(r, 10.0f);
        g.setGradientFill(juce::ColourGradient(accent.withAlpha(on ? 0.30f : 0.08f), r.getX(), r.getY(),
                                               accent.withAlpha(0.0f), r.getX(), r.getY() + 230.0f, false));
        g.fillRoundedRectangle(r, 10.0f);

        const auto body = r.withTrimmedTop(80.0f).reduced(6.0f, 0.0f).withTrimmedBottom(6.0f);
        g.setColour(juce::Colour(0xff0c0f14).withAlpha(0.8f));
        g.fillRoundedRectangle(body, 8.0f);

        g.setColour(accent.withAlpha(on ? 0.5f : 0.18f));
        g.drawRoundedRectangle(r.reduced(0.6f), 10.0f, 1.4f);

        g.setColour(on ? juce::Colours::white : textDim);
        g.setFont(uiFont(30.0f, true));
        g.drawText(title, r.withHeight(80.0f), juce::Justification::centred, false);

        g.setColour(on ? accent : textDim);
        g.setFont(uiFont(19.0f, true));
        g.drawText(on ? "ON" : "OFF", r.withHeight(80.0f).withTrimmedRight(22.0f), juce::Justification::centredRight, false);
    }

    void drawCompMeter(juce::Graphics& g)
    {
        g.setColour(textMain.withAlpha(power[1].getToggleState() ? 1.0f : 0.4f));
        g.setFont(uiFont(17.0f, false, 0.06f));
        g.drawText("GAIN REDUCTION", juce::Rectangle<float>(400.0f, 216.0f, 323.0f, 24.0f), juce::Justification::centred, false);

        constexpr int segments = 16;
        const auto area = juce::Rectangle<float>(427.0f, 252.0f, 270.0f, 22.0f);
        const auto width = area.getWidth() / segments;
        const auto lit = juce::roundToInt(shownReduction / 1.25f);
        g.setColour(juce::Colour(0xff0a0c10));
        g.fillRoundedRectangle(area.expanded(3.0f), 4.0f);
        for (int i = 0; i < segments; ++i)
        {
            g.setColour(i < lit ? green : juce::Colour(0xff1f242c));
            g.fillRoundedRectangle(juce::Rectangle<float>(area.getX() + i * width + 1.5f, area.getY(), width - 3.0f, area.getHeight()), 2.0f);
        }
    }

    void drawEqScale(juce::Graphics& g)
    {
        g.setColour(textDim);
        g.setFont(uiFont(16.0f));
        const auto x = [this](double f) { return 40.0f + static_cast<float>(std::log(f / 20.0) / std::log(1000.0)) * graph.getWidth(); };
        const std::pair<double, const char*> marks[] { { 20.0, "20" }, { 100.0, "100" }, { 1000.0, "1K" }, { 10000.0, "10K" } };
        for (const auto& [f, text] : marks)
            g.drawText(text, juce::Rectangle<float>(x(f) - 20.0f + (f == 20.0 ? 16.0f : 0.0f), 446.0f, 40.0f, 22.0f),
                       juce::Justification::centred, false);
    }

    void drawOutput(juce::Graphics& g)
    {
        const auto r = juce::Rectangle<float>(1396.0f, 120.0f, 123.0f, 782.0f);
        g.setColour(juce::Colour(0xff10131a));
        g.fillRoundedRectangle(r, 10.0f);
        g.setColour(juce::Colour(0xff1f242d));
        g.drawRoundedRectangle(r.reduced(0.6f), 10.0f, 1.2f);

        g.setColour(textMain);
        g.setFont(uiFont(22.0f, false, 0.05f));
        g.drawText("OUTPUT", juce::Rectangle<float>(1396.0f, 150.0f, 123.0f, 32.0f), juce::Justification::centred, false);

        g.setColour(juce::Colour(0xff0a0c10));
        g.fillRoundedRectangle(juce::Rectangle<float>(1426.0f, 198.0f, 36.0f, 418.0f), 6.0f);
        for (float y = 598.0f; y > 208.0f; y -= 14.0f)
        {
            const auto segmentDb = meterDb(y - 5.0f);
            const auto lit = meterLevel >= segmentDb;
            auto colour = segmentDb >= -3.0f ? juce::Colour(0xffef4444) : segmentDb >= -9.0f ? juce::Colour(0xfff5c518) : juce::Colour(0xff27d05a);
            g.setColour(lit ? colour : juce::Colour(0xff262b33));
            g.fillRect(juce::Rectangle<float>(1433.0f, y - 11.0f, 22.0f, 11.0f));
        }

        g.setColour(textDim);
        g.setFont(uiFont(15.0f));
        for (const auto db : { 0, -6, -12, -24, -36, -60 })
            g.drawText(juce::String(db), juce::Rectangle<float>(1472.0f, meterY(static_cast<float>(db)) - 10.0f, 40.0f, 20.0f),
                       juce::Justification::centredLeft, false);
    }

    void drawKnobText(juce::Graphics& g)
    {
        for (const auto& knob : knobs)
        {
            const auto on = knob.module == 4 || power[knob.module].getToggleState();
            if (knob.label.isNotEmpty())
            {
                g.setColour(textMain.withAlpha(on ? 1.0f : 0.4f));
                g.setFont(uiFont(18.0f, false, 0.04f));
                g.drawText(knob.label, juce::Rectangle<float>(knob.centre.x - 90.0f, knob.centre.y - knob.size * 0.5f - 34.0f, 180.0f, 24.0f),
                           juce::Justification::centred, false);
            }
            g.setColour(textMain.withAlpha(on ? 1.0f : 0.4f));
            g.setFont(uiFont(knob.size > 120.0f ? 25.0f : 22.0f));
            g.drawText(valueText(knob), juce::Rectangle<float>(knob.centre.x - 90.0f, knob.centre.y + knob.size * 0.5f + 1.0f, 180.0f, 30.0f),
                       juce::Justification::centred, false);
        }
    }

    void drawFooter(juce::Graphics& g)
    {
        g.setColour(juce::Colour(0xff0c0f14));
        g.fillRect(0.0f, 928.0f, canvasWidth, 96.0f);
        g.setColour(juce::Colour(0xff1c2129));
        g.drawHorizontalLine(928, 0.0f, canvasWidth);

        g.setColour(textDim);
        g.setFont(uiFont(15.0f, false, 0.42f));
        g.drawText("AMANORSAC.STUDIO", juce::Rectangle<float>(56.0f, 960.0f, 400.0f, 24.0f), juce::Justification::centredLeft, false);
        g.setFont(uiFont(15.0f, false, 0.5f));
        g.drawText(juce::String(juce::CharPointer_UTF8("CREATE  \xe2\x80\xa2  PLAY  \xe2\x80\xa2  PERFORM")),
                   juce::Rectangle<float>(940.0f, 960.0f, 544.0f, 24.0f), juce::Justification::centredRight, false);

        g.setColour(juce::Colour(0xffb6bec9));
        g.setFont(uiFont(17.0f, false, 0.55f));
        g.drawText("PERFORM LIVE", juce::Rectangle<float>(518.0f, 944.0f, 500.0f, 24.0f), juce::Justification::centred, false);
        g.setColour(textDim.withAlpha(0.7f));
        g.setFont(uiFont(13.0f, false, 0.7f));
        g.drawText("MUSIC LIVES HERE", juce::Rectangle<float>(518.0f, 970.0f, 500.0f, 22.0f), juce::Justification::centred, false);
    }

    juce::String valueText(const Knob& knob) const
    {
        const auto v = static_cast<float>(knob.slider->getValue());
        const auto& id = knob.id;
        if (id.startsWith("eq_")) return std::abs(v) < 0.05f ? "0 dB" : (v > 0 ? "+" : "") + juce::String(v, 1) + " dB";
        if (id == "comp_threshold") return juce::String(juce::roundToInt(v)) + " dB";
        if (id == "comp_ratio") return trimmed(v, 1) + ":1";
        if (id == "comp_attack") return (v < 1.0f ? juce::String(v, 1) : juce::String(juce::roundToInt(v))) + " ms";
        if (id == "comp_release" || id == "rev_predelay") return juce::String(juce::roundToInt(v)) + " ms";
        if (id == "comp_makeup") return trimmed(v, 1) + " dB";
        if (id == "rev_decay") return juce::String(v, 1) + " s";
        if (id == "dly_time")
        {
            const auto division = delayDivision.getSelectedItemIndex();
            return division >= 0 && division != freeDivision ? delayDivisionNames()[division] : juce::String(juce::roundToInt(v)) + " ms";
        }
        if (id == "dly_filter") return v >= 1000.0f ? juce::String(v / 1000.0f, 1) + " kHz" : juce::String(juce::roundToInt(v)) + " Hz";
        if (id == "out_gain") return juce::String(v, 1) + " dB";
        return juce::String(juce::roundToInt(v)) + " %";
    }

    PerformProcessor& processor;
    Look look;
    EqGraph graph;
    std::array<PowerButton, 4> power;
    std::vector<Knob> knobs;
    juce::ComboBox reverbType, delayDivision;
    PillToggle pingPong;
    MuteButton mute;
    ChevronButton prev, next;
    ClickArea presetName;
    DotsButton dots;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAttachments;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAttachments;

    const juce::Rectangle<float> panels[4] { { 24.0f, 120.0f, 363.0f, 782.0f }, { 400.0f, 120.0f, 323.0f, 782.0f },
                                             { 737.0f, 120.0f, 317.0f, 782.0f }, { 1068.0f, 120.0f, 315.0f, 782.0f } };
    juce::String shownPreset;
    float shownReduction = 0.0f, meterLevel = -90.0f;
};

// ------------------------------------------------------------------ editor
PerformEditor::PerformEditor(PerformProcessor& owner)
    : AudioProcessorEditor(owner), surface(std::make_unique<Surface>(owner))
{
    addAndMakeVisible(*surface);
    // Power buttons sit where the picture puts them; they are laid out here
    // because the surface owns every other position.
    setResizable(true, true);
    setResizeLimits(768, 512, 1920, 1280);
    if (auto* limits = getConstrainer()) limits->setFixedAspectRatio(canvasWidth / canvasHeight);
    setSize(1152, 768);
    startTimerHz(30);
}

PerformEditor::~PerformEditor() { stopTimer(); }

void PerformEditor::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff0a0c10)); }

void PerformEditor::resized()
{
    surface->setTransform(juce::AffineTransform::scale(static_cast<float>(getWidth()) / canvasWidth));
}

void PerformEditor::timerCallback() { surface->tick(); }
}
