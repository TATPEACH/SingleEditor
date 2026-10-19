#include "UI/Components/ParametricEqMagComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernEqKnobPainter.h"
#include "UI/LookAndFeel/ModernPanelPainter.h"

namespace singleeditor
{
namespace
{
constexpr std::array<std::size_t, 3> simpleBandIndices { 1, 5, 6 };
constexpr std::array<const char*, 3> simpleBandNames { "LOW", "PRESENCE", "AIR" };

juce::String compactFrequencyLabel(double frequencyHz)
{
    if (frequencyHz >= 1000.0)
    {
        const auto kilohertz = frequencyHz / 1000.0;
        return juce::String(kilohertz,
                            kilohertz < 10.0 && std::fmod(kilohertz, 1.0) != 0.0 ? 1 : 0)
             + " kHz";
    }
    return juce::String(juce::roundToInt(frequencyHz)) + " Hz";
}

class CompactEqKnobLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
        if (!RendererVisualStyle::isClassic()
            && static_cast<bool>(slider.getProperties().getWithDefault("modernInlineGain", false)))
        {
            label->setBorderSize(juce::BorderSize<int>(0));
            label->setFont(ModernTypography::font(10.0f));
            label->setMinimumHorizontalScale(0.9f);
        }
        return label;
    }

    void drawRotarySlider(juce::Graphics& g,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPosition,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& slider) override
    {
        if (!RendererVisualStyle::isClassic())
        {
            if (slider.getProperties().contains("eqLiveValue"))
                sliderPosition = static_cast<float>(slider.valueToProportionOfLength(
                    static_cast<double>(slider.getProperties()["eqLiveValue"])));
            ModernEqKnobPainter::draw(g, juce::Rectangle<int>(x, y, width, height).toFloat(),
                                      sliderPosition, rotaryStartAngle, rotaryEndAngle, slider);
            return;
        }
        static const std::array<juce::Colour, ParametricEqParameters::bandCount> bandColours {
            juce::Colour(0xffdf7a52),
            juce::Colour(0xffd69a43),
            juce::Colour(0xff68a971),
            juce::Colour(0xff2ea39c),
            juce::Colour(0xff348fa8),
            juce::Colour(0xff517eb1),
            juce::Colour(0xff806cad),
            juce::Colour(0xffa75f8d)
        };
        const auto band = std::clamp(static_cast<int>(slider.getProperties()["eqBandIndex"]),
                                     0,
                                     static_cast<int>(bandColours.size() - 1));
        const auto accent = bandColours[static_cast<std::size_t>(band)];
        auto area = juce::Rectangle<float>(static_cast<float>(x),
                                           static_cast<float>(y),
                                           static_cast<float>(width),
                                           static_cast<float>(height));
        // JUCE already removes the textbox from the rotary drawing area. Keep
        // the dial close to that available diameter so S remains a real VST-
        // style control rather than a tiny icon surrounded by dead space.
        const auto diameter = std::max(18.0f, std::min(area.getWidth(), area.getHeight()) - 2.0f);
        auto dial = juce::Rectangle<float>(diameter, diameter).withCentre(area.getCentre());
        const auto radius = dial.getWidth() * 0.5f;
        const auto centre = dial.getCentre();
        const auto angle = rotaryStartAngle
                         + sliderPosition * (rotaryEndAngle - rotaryStartAngle);

        juce::Path track;
        track.addCentredArc(centre.x,
                            centre.y,
                            radius - 2.0f,
                            radius - 2.0f,
                            0.0f,
                            rotaryStartAngle,
                            rotaryEndAngle,
                            true);
        g.setColour(RendererVisualStyle::borderColour().withAlpha(0.7f));
        g.strokePath(track, juce::PathStrokeType(2.0f,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

        juce::Path valueArc;
        valueArc.addCentredArc(centre.x,
                               centre.y,
                               radius - 2.0f,
                               radius - 2.0f,
                               0.0f,
                               rotaryStartAngle,
                               angle,
                               true);
        g.setColour(accent.withAlpha(slider.isEnabled() ? 0.96f : 0.36f));
        g.strokePath(valueArc, juce::PathStrokeType(3.2f,
                                                   juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        constexpr auto tickCount = 9;
        for (auto tick = 0; tick < tickCount; ++tick)
        {
            const auto proportion = static_cast<float>(tick) / static_cast<float>(tickCount - 1);
            const auto tickAngle = rotaryStartAngle
                                 + proportion * (rotaryEndAngle - rotaryStartAngle);
            const auto tickLength = tick == tickCount / 2 ? 3.4f : 2.2f;
            const auto outer = centre + juce::Point<float>(std::sin(tickAngle),
                                                           -std::cos(tickAngle)) * (radius + 1.2f);
            const auto inner = centre + juce::Point<float>(std::sin(tickAngle),
                                                           -std::cos(tickAngle)) * (radius + 1.2f - tickLength);
            g.setColour(tick == tickCount / 2
                            ? RendererVisualStyle::primaryText().withAlpha(0.62f)
                            : RendererVisualStyle::secondaryText().withAlpha(0.34f));
            g.drawLine({ inner, outer }, tick == tickCount / 2 ? 1.35f : 0.8f);
        }

        auto shadow = dial.reduced(3.5f).translated(0.0f, 1.8f);
        g.setColour(juce::Colours::black.withAlpha(0.24f));
        g.fillEllipse(shadow);

        auto face = dial.reduced(3.5f);
        juce::ColourGradient faceGradient(juce::Colour(0xff424a4c),
                                          face.getCentreX(),
                                          face.getY(),
                                          juce::Colour(0xff151b1d),
                                          face.getCentreX(),
                                          face.getBottom(),
                                          false);
        faceGradient.addColour(0.58, juce::Colour(0xff282f31));
        g.setGradientFill(faceGradient);
        g.fillEllipse(face);
        g.setColour(juce::Colour(0xff080c0d).withAlpha(0.92f));
        g.drawEllipse(face, 1.2f);
        g.setColour(juce::Colours::white.withAlpha(0.09f));
        g.drawEllipse(face.reduced(1.6f), 0.8f);

        const auto pointerStart = centre + juce::Point<float>(std::sin(angle),
                                                              -std::cos(angle)) * 3.0f;
        const auto pointerEnd = centre + juce::Point<float>(std::sin(angle),
                                                            -std::cos(angle)) * (radius - 7.0f);
        g.setColour(accent.brighter(0.12f));
        g.drawLine({ pointerStart, pointerEnd }, 2.0f);
        g.setColour(juce::Colour(0xff070a0b));
        g.fillEllipse(juce::Rectangle<float>(3.2f, 3.2f).withCentre(centre));

        if (slider.hasKeyboardFocus(false))
        {
            g.setColour(accent.withAlpha(0.52f));
            g.drawEllipse(dial.expanded(1.5f), 1.2f);
        }
    }
};

juce::String filterTypeName(ParametricEqFilterType type)
{
    switch (type)
    {
        case ParametricEqFilterType::highPass: return "HP";
        case ParametricEqFilterType::lowShelf: return "Low Shelf";
        case ParametricEqFilterType::bell: return "Bell";
        case ParametricEqFilterType::notch: return "Notch";
        case ParametricEqFilterType::highShelf: return "High Shelf";
        case ParametricEqFilterType::lowPass: return "LP";
    }
    return "Bell";
}
}

ParametricEqMagComponent::NumericSlider::~NumericSlider()
{
    abandonEditor();
    for (auto* child : getChildren())
        if (auto* label = dynamic_cast<juce::Label*>(child))
        {
            label->onEditorShow = {};
            label->onEditorHide = {};
        }
}

void ParametricEqMagComponent::NumericSlider::abandonEditor()
{
    cancelPendingUpdate();
    if (activeEditor != nullptr)
    {
        activeEditor->onTextChange = {};
        activeEditor->removeKeyListener(this);
    }
    activeEditor = nullptr;
    lastLiveValue.reset();
    getProperties().remove("eqLiveValue");
    hideTextBox(true);
}

void ParametricEqMagComponent::NumericSlider::lookAndFeelChanged()
{
    // A style/theme change replaces JUCE's Label. Complete its live draft before
    // replacement, just as leaving the field does, so no orphan callback survives.
    const juce::Component::SafePointer<NumericSlider> safe(this);
    if (activeEditor != nullptr) hideTextBox(false);
    if (safe != nullptr) juce::Slider::lookAndFeelChanged();
}

void ParametricEqMagComponent::NumericSlider::childrenChanged()
{
    // Slider recreates its Label when its style or LookAndFeel changes. Attach
    // to each new editor, including compact rotary and Advanced stepper fields.
    for (auto* child : getChildren())
        if (auto* label = dynamic_cast<juce::Label*>(child))
        {
            label->onEditorShow = [safe = juce::Component::SafePointer<NumericSlider>(this),
                                   valueLabel = juce::Component::SafePointer<juce::Label>(label)]
            {
                if (safe != nullptr && valueLabel != nullptr && !RendererVisualStyle::isClassic())
                    if (auto* editor = valueLabel->getCurrentTextEditor())
                    {
                        safe->activeEditor = editor;
                        safe->lastLiveValue = safe->getValue();
                        editor->addKeyListener(safe.getComponent());
                        editor->onTextChange = [safe]
                        {
                            if (safe != nullptr) safe->triggerAsyncUpdate();
                        };
                        if (safe->onEditBegin) safe->onEditBegin();
                    }
            };
            label->onEditorHide = [safe = juce::Component::SafePointer<NumericSlider>(this)]
            {
                if (safe != nullptr) safe->finishEditor(false);
            };
        }
}

std::optional<double> ParametricEqMagComponent::NumericSlider::parseDraft()
{
    if (activeEditor == nullptr) return {};
    auto text = activeEditor->getText().trim();
    const auto bytes = text.toRawUTF8();
    char* end = nullptr;
    const auto parsed = std::strtod(bytes, &end);
    if (end == bytes || !std::isfinite(parsed)) return {};
    if (juce::String(bytes, static_cast<std::size_t>(end - bytes)).containsAnyOf("xXpP")) return {};
    const auto unit = juce::String(end).trim();
    const bool frequency = getComponentID().endsWith(".frequency-hz");
    if (unit.isNotEmpty() && !unit.equalsIgnoreCase(getTextValueSuffix().trim())
        && !(frequency && (unit.equalsIgnoreCase("Hz") || unit.equalsIgnoreCase("k")
                            || unit.equalsIgnoreCase("kHz"))))
        return {};
    const auto value = getValueFromText(text);
    if (!std::isfinite(value)) return {};
    return getNormalisableRange().snapToLegalValue(value);
}

void ParametricEqMagComponent::NumericSlider::handleAsyncUpdate()
{
    if (activeEditor == nullptr || !isEnabled() || activeEditor->isReadOnly()) return;
    const auto value = parseDraft();
    if (!value.has_value() || value == lastLiveValue) return;
    lastLiveValue = value;
    getProperties().set("eqLiveValue", *value);
    repaint();
    // One pending update per editor; fast text/arrow bursts use the latest text.
    // The existing application callback updates Preview only if already active.
    if (onLiveValueChange) onLiveValueChange(*value);
}

void ParametricEqMagComponent::NumericSlider::finishEditor(bool cancelled)
{
    if (activeEditor == nullptr) return;
    const juce::Component::SafePointer<NumericSlider> safe(this);
    cancelPendingUpdate();
    if (!cancelled)
    {
        handleAsyncUpdate();
        if (safe == nullptr) return;
        if (activeEditor != nullptr && !parseDraft().has_value())
            activeEditor->setText(getTextFromValue(lastLiveValue.value_or(getValue())), false);
    }
    if (activeEditor != nullptr)
    {
        activeEditor->onTextChange = {};
        activeEditor->removeKeyListener(this);
    }
    activeEditor = nullptr;
    lastLiveValue.reset();
    getProperties().remove("eqLiveValue");
    repaint();
    if (onEditEnd) onEditEnd(cancelled);
}

bool ParametricEqMagComponent::NumericSlider::keyPressed(const juce::KeyPress& key,
                                                        juce::Component* origin)
{
    auto* editor = dynamic_cast<juce::TextEditor*>(origin);
    if (RendererVisualStyle::isClassic() || !isEnabled() || editor == nullptr
        || !isParentOf(editor) || editor->isReadOnly()
        || key.getModifiers().isAnyModifierKeyDown())
        return false;

    if (key == juce::KeyPress::escapeKey)
    {
        finishEditor(true);
        return false;
    }
    if (key == juce::KeyPress::returnKey)
    {
        if (!parseDraft().has_value())
            editor->setText(getTextFromValue(lastLiveValue.value_or(getValue())), false);
        return false;
    }
    if (key != juce::KeyPress::upKey && key != juce::KeyPress::downKey) return false;
    auto value = parseDraft().value_or(lastLiveValue.value_or(getValue()));
    value = getNormalisableRange().snapToLegalValue(
        value + (key == juce::KeyPress::upKey ? getInterval() : -getInterval()));
    // Slider::setValue would destroy the editor; publish the draft separately.
    editor->setText(getTextFromValue(value), false);
    editor->selectAll();
    triggerAsyncUpdate();
    return true;
}

ParametricEqMagComponent::ParametricEqMagComponent(ParametricEqParameters initialParameters)
    : parameters(std::move(initialParameters))
{
    compactKnobLookAndFeel = std::make_unique<CompactEqKnobLookAndFeel>();
    setInterceptsMouseClicks(true, true);
    setWantsKeyboardFocus(true);
    setDescription("Eight-band AudioSuite equalizer. Preview auditions without changing the document; Apply creates one undoable edit.");

    configureButton(simpleButton, "Simple", "eq.mode.simple");
    configureButton(advancedButton, "Advanced", "eq.mode.advanced");
    configureButton(smallButton, "S", "eq.size.small");
    configureButton(mediumButton, "M", "eq.size.medium");
    configureButton(largeButton, "L", "eq.size.large");
    configureButton(previewButton, "Preview", "eq.preview");
    configureButton(applyButton, "Apply", "eq.apply");

    simpleButton.onClick = [this] { setViewMode(ViewMode::simple); };
    advancedButton.onClick = [this] { setViewMode(ViewMode::advanced); };
    smallButton.onClick = [this] { requestMagSize(RendererMagSize::small); };
    mediumButton.onClick = [this] { requestMagSize(RendererMagSize::medium); };
    largeButton.onClick = [this] { requestMagSize(RendererMagSize::large); };
    previewButton.onClick = [this]
    {
        if (!applying && onPreviewRequested)
            onPreviewRequested(!previewing);
    };
    applyButton.onClick = [this]
    {
        if (!applying && onApplyRequested)
            onApplyRequested();
    };

    presetBox.setComponentID("eq.preset");
    presetBox.setTextWhenNothingSelected("Custom");
    presetBox.setName("Parametric EQ Preset");
    presetBox.setTitle("Parametric EQ sound preset");
    presetBox.setDescription("Changes sound parameters only; layout and view mode remain unchanged.");
    auto itemId = 1;
    for (const auto& preset : ParametricEqProcessor::builtInPresets())
        presetBox.addItem(preset.displayName, itemId++);
    presetBox.onChange = [this] { handlePresetChanged(); };
    addAndMakeVisible(presetBox);

    advancedBandBox.setComponentID("eq.advanced.band");
    advancedBandBox.setName("Advanced EQ band");
    advancedBandBox.setTitle("Choose one of eight EQ bands to edit");
    for (auto band = 1; band <= static_cast<int>(ParametricEqParameters::bandCount); ++band)
        advancedBandBox.addItem("Band " + juce::String(band), band);
    advancedBandBox.setSelectedId(1, juce::dontSendNotification);
    advancedBandBox.onChange = [this]
    {
        setSelectedAdvancedBand(static_cast<std::size_t>(
            std::max(0, advancedBandBox.getSelectedId() - 1)));
    };
    addAndMakeVisible(advancedBandBox);

    for (std::size_t index = 0; index < simpleLabels.size(); ++index)
    {
        auto& label = simpleLabels[index];
        label.setText(simpleBandNames[index], juce::dontSendNotification);
        ModernAppleLookAndFeel::setLabelRole(label, ModernAppleLookAndFeel::LabelRole::secondary);
        label.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(label);

        auto& slider = simpleGainSliders[index];
        configureSlider(slider,
                        -24.0,
                        24.0,
                        0.1,
                        " dB",
                        juce::String(simpleBandNames[index]) + " gain");
        slider.setComponentID("eq.simple.gain." + juce::String(static_cast<int>(index + 1)));
        slider.onValueChange = [this, index] { handleSimpleGainChanged(index); };
        slider.onLiveValueChange = [this, index](double value)
        {
            handleNumericValueChanged(simpleBandIndices[index], NumericField::gain, value, true);
        };
    }

    for (std::size_t index = 0; index < bandControls.size(); ++index)
        configureBandControls(index);

    updateControlsFromModel();
    updateVisibilityAndLayout();
}

ParametricEqMagComponent::~ParametricEqMagComponent()
{
    abandonNumericEditors();
}

void ParametricEqMagComponent::abandonNumericEditors()
{
    numericEditStart.reset();
    for (auto& slider : simpleGainSliders) slider.abandonEditor();
    for (auto& controls : bandControls)
        for (auto* slider : { &controls.frequency, &controls.gain, &controls.q })
            slider->abandonEditor();
}

void ParametricEqMagComponent::finishNumericEdit(bool cancelled)
{
    if (!numericEditStart.has_value()) return;
    const auto initial = *numericEditStart;
    numericEditStart.reset();
    const bool changed = cancelled && parameters != initial;
    if (cancelled) parameters = initial;
    updateControlsFromModel();
    if (changed && onParametersChanged) onParametersChanged(parameters);
    repaint();
}

void ParametricEqMagComponent::configureButton(juce::TextButton& button,
                                               const juce::String& text,
                                               const juce::String& componentId)
{
    button.setButtonText(text);
    button.setComponentID(componentId);
    button.setName(text);
    button.setTitle(text);
    addAndMakeVisible(button);
}

void ParametricEqMagComponent::configureSlider(NumericSlider& slider,
                                               double minimum,
                                               double maximum,
                                               double interval,
                                               const juce::String& suffix,
                                               const juce::String& name)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 22);
    slider.setRange(minimum, maximum, interval);
    slider.setTextValueSuffix(suffix);
    slider.setName(name);
    slider.setTitle(name);
    slider.onEditBegin = [this] { numericEditStart = parameters; };
    slider.onEditEnd = [this](bool cancelled) { finishNumericEdit(cancelled); };
    addAndMakeVisible(slider);
}

void ParametricEqMagComponent::configureBandControls(std::size_t index)
{
    auto& controls = bandControls[index];
    const auto bandNumber = juce::String(static_cast<int>(index + 1));
    controls.enabled.setButtonText("B" + bandNumber);
    controls.enabled.setComponentID("eq.band" + bandNumber + ".enabled");
    controls.enabled.setName("Band " + bandNumber + " enabled");
    controls.enabled.setTitle("Band " + bandNumber + " enabled");
    controls.enabled.onClick = [this, index] { handleBandControlChanged(index); };
    addAndMakeVisible(controls.enabled);

    controls.type.setComponentID("eq.band" + bandNumber + ".type");
    controls.type.setName("Band " + bandNumber + " filter type");
    for (auto typeId = 1; typeId <= 6; ++typeId)
        controls.type.addItem(filterTypeName(filterTypeForId(typeId)), typeId);
    controls.type.onChange = [this, index] { handleBandControlChanged(index); };
    addAndMakeVisible(controls.type);

    configureSlider(controls.frequency, 10.0, 100000.0, 1.0, " Hz", "Band " + bandNumber + " frequency");
    controls.frequency.setSkewFactorFromMidPoint(1000.0);
    if (!RendererVisualStyle::isClassic())
    {
        controls.frequency.setTextValueSuffix("");
        controls.frequency.textFromValueFunction = [](double hz)
        {
            return juce::String(juce::roundToInt(hz));
        };
        controls.frequency.valueFromTextFunction = [](const juce::String& text)
        {
            return text.trim().getDoubleValue() * (text.containsIgnoreCase("k") ? 1000.0 : 1.0);
        };
        controls.frequency.setTooltip("Frequency in Hz; k means 1000 Hz. Enter a value or drag the stepper.");
    }
    controls.frequency.setComponentID("eq.band" + bandNumber + ".frequency-hz");
    controls.frequency.onValueChange = [this, index]
    {
        handleNumericValueChanged(index, NumericField::frequency, bandControls[index].frequency.getValue());
    };
    controls.frequency.onLiveValueChange = [this, index](double value)
    {
        handleNumericValueChanged(index, NumericField::frequency, value);
    };

    configureSlider(controls.gain, -24.0, 24.0, 0.1, " dB", "Band " + bandNumber + " gain");
    controls.gain.setComponentID("eq.band" + bandNumber + ".gain-db");
    controls.gain.getProperties().set("eqBandIndex", static_cast<int>(index));
    controls.gain.onValueChange = [this, index]
    {
        handleNumericValueChanged(index, NumericField::gain, bandControls[index].gain.getValue());
    };
    controls.gain.onLiveValueChange = [this, index](double value)
    {
        handleNumericValueChanged(index, NumericField::gain, value);
    };

    configureSlider(controls.q, 0.1, 18.0, 0.01, " Q", "Band " + bandNumber + " Q");
    controls.q.setSkewFactorFromMidPoint(1.0);
    controls.q.setComponentID("eq.band" + bandNumber + ".q");
    controls.q.onValueChange = [this, index]
    {
        handleNumericValueChanged(index, NumericField::q, bandControls[index].q.getValue());
    };
    controls.q.onLiveValueChange = [this, index](double value)
    {
        handleNumericValueChanged(index, NumericField::q, value);
    };

    controls.slope.setComponentID("eq.band" + bandNumber + ".slope");
    controls.slope.setName("Band " + bandNumber + " slope");
    controls.slope.addItem("12 dB", 1);
    controls.slope.addItem("24 dB", 2);
    controls.slope.onChange = [this, index] { handleBandControlChanged(index); };
    addAndMakeVisible(controls.slope);
}

void ParametricEqMagComponent::setParameters(const ParametricEqParameters& next)
{
    abandonNumericEditors();
    parameters = next;
    updateControlsFromModel();
    repaint();
}

const ParametricEqParameters& ParametricEqMagComponent::getParameters() const noexcept
{
    return parameters;
}

ParametricEqMagComponent::ViewMode ParametricEqMagComponent::getViewMode() const noexcept
{
    return viewMode;
}

void ParametricEqMagComponent::setViewMode(ViewMode mode, bool requestSweetSpot)
{
    if (viewMode == mode)
        return;
    viewMode = mode;
    if (onViewModeChanged)
        onViewModeChanged(viewMode);
    if (requestSweetSpot)
        requestMagSize(mode == ViewMode::simple ? RendererMagSize::medium
                                                : RendererMagSize::large);
    updateVisibilityAndLayout();
    repaint();
}

std::size_t ParametricEqMagComponent::getSelectedAdvancedBand() const noexcept
{
    return selectedAdvancedBand;
}

void ParametricEqMagComponent::setSelectedAdvancedBand(std::size_t band)
{
    band = std::min(band, ParametricEqParameters::bandCount - 1);
    if (selectedAdvancedBand == band)
        return;
    selectedAdvancedBand = band;
    advancedBandBox.setSelectedId(static_cast<int>(band) + 1, juce::dontSendNotification);
    if (onSelectedAdvancedBandChanged)
        onSelectedAdvancedBandChanged(band);
    updateVisibilityAndLayout();
    repaint();
}

void ParametricEqMagComponent::setPreviewing(bool shouldPreview)
{
    previewing = shouldPreview;
    previewButton.setButtonText(previewing ? "Stop Preview" : "Preview");
    previewButton.setName(previewing ? "Stop Preview" : "Preview");
    previewButton.setTitle(previewing ? "Stop Parametric EQ Preview"
                                     : "Start Parametric EQ Preview");
    previewButton.setToggleState(previewing, juce::dontSendNotification);
    // Optional skins mark a Mag that is currently audible as a preview.
    getProperties().set("skinLive", previewing);
    repaint();
}

void ParametricEqMagComponent::setApplying(bool shouldApply)
{
    applying = shouldApply;
    applyButton.setButtonText(applying ? "Applying..." : "Apply");
    applyButton.setEnabled(!applying);
    previewButton.setEnabled(!applying);
    repaint();
}

void ParametricEqMagComponent::setStatusText(juce::String status)
{
    statusText = std::move(status);
    repaint();
}

void ParametricEqMagComponent::setExactMagnitudeCurve(std::vector<float> magnitudeDb, double minimumHz, double maximumHz)
{
    if (!std::isfinite(minimumHz) || !std::isfinite(maximumHz) || minimumHz <= 0.0 || maximumHz <= minimumHz)
    {
        clearExactMagnitudeCurve();
        return;
    }
    curveMinimumHz = minimumHz;
    curveMaximumHz = maximumHz;
    exactMagnitudeDb = std::move(magnitudeDb);
    refreshBandCurves();
    repaint(curveBounds);
}

void ParametricEqMagComponent::clearExactMagnitudeCurve()
{
    exactMagnitudeDb.clear();
    refreshCoefficientCurve();
    repaint(curveBounds);
}

void ParametricEqMagComponent::setSourceSampleRate(double sampleRate)
{
    sourceSampleRate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 0.0;
    refreshCoefficientCurve();
    repaint(curveBounds);
}

void ParametricEqMagComponent::refreshCoefficientCurve()
{
    if (RendererVisualStyle::isClassic()) return;
    for (std::size_t i = 0; i < bandControls.size(); ++i)
        bandControls[i].gain.setTooltip("Band " + juce::String(static_cast<int>(i + 1)) + " | "
            + juce::String(juce::roundToInt(parameters.bands[i].frequencyHz)) + " Hz | Gain (dB)");
    curveMinimumHz = 20.0;
    curveMaximumHz = std::min(20000.0, sourceSampleRate * 0.49);
    exactMagnitudeDb = ParametricEqProcessor::magnitudeResponse(
        parameters, sourceSampleRate, curveMinimumHz, curveMaximumHz);
    refreshBandCurves();
}

void ParametricEqMagComponent::refreshBandCurves()
{
    for (auto& curve : bandMagnitudeDb) curve.clear();
    if (RendererVisualStyle::isClassic() || exactMagnitudeDb.size() < 2) return;

    // Evaluate only on parameter/source changes, never during paint or resize.
    // Isolating a band reuses the exact audio coefficient resolver, including
    // pass filters, cascaded slopes and the source's effective frequency limit.
    auto isolated = parameters;
    for (auto& band : isolated.bands) band.enabled = false;
    for (std::size_t i = 0; i < parameters.bands.size(); ++i)
    {
        if (!parameters.bands[i].enabled) continue;
        isolated.bands[i].enabled = true;
        bandMagnitudeDb[i] = ParametricEqProcessor::magnitudeResponse(
            isolated, sourceSampleRate, curveMinimumHz, curveMaximumHz);
        isolated.bands[i].enabled = false;
    }
}

void ParametricEqMagComponent::applyWorkspaceVisualContract(RendererDensity nextDensity,
                                                            RendererMagSize nextSize,
                                                            RendererInformationTier nextTier,
                                                            bool nextSweetSpot)
{
    density = nextDensity;
    magSize = nextSize;
    informationTier = nextTier;
    sweetSpot = nextSweetSpot;
    updateVisibilityAndLayout();
    repaint();
}

void ParametricEqMagComponent::paint(juce::Graphics& g)
{
    if (!RendererVisualStyle::isClassic())
    {
        paintModern(g);
        return;
    }
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(RendererVisualStyle::panelBackground());
    g.fillRoundedRectangle(bounds.reduced(0.5f), RendererVisualStyle::cardRadius());

    auto content = getLocalBounds().reduced(informationTier == RendererInformationTier::summary ? 10 : 14);
    if (informationTier == RendererInformationTier::summary)
    {
        g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        for (std::size_t index = 0; index < bandControls.size(); ++index)
        {
            const auto sliderBounds = bandControls[index].gain.getBounds();
            if (!sliderBounds.isEmpty())
            {
                const auto enabled = parameters.bands[index].enabled;
                g.setColour(enabled ? RendererVisualStyle::primaryText()
                                    : RendererVisualStyle::secondaryText().withAlpha(0.72f));
                g.drawText(compactFrequencyLabel(parameters.bands[index].frequencyHz),
                           sliderBounds.withY(sliderBounds.getY() - 12).withHeight(11),
                           juce::Justification::centred);
            }
        }
        return;
    }

    g.setColour(RendererVisualStyle::primaryText());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::titleTextSize(), juce::Font::bold)));
    g.drawText(informationTier == RendererInformationTier::summary ? "EQ" : "Parametric EQ",
               content.removeFromTop(24),
               juce::Justification::centredLeft);

    g.setColour(RendererVisualStyle::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::captionTextSize())));
    auto status = statusText;
    if (!sweetSpot)
        status += " | Resize for the selected view";
    g.drawText(status,
               content.removeFromTop(18),
               juce::Justification::centredLeft,
               true);

    if (viewMode == ViewMode::advanced && informationTier != RendererInformationTier::detail)
    {
        g.setColour(RendererVisualStyle::secondaryText());
        g.drawText("Advanced uses the L sweet spot. Choose L to expose all eight bands.",
                   content.withTrimmedTop(46).removeFromTop(34),
                   juce::Justification::centredLeft,
                   true);
    }

    paintCurve(g);
}

void ParametricEqMagComponent::paintModern(juce::Graphics& g)
{
    if (informationTier != RendererInformationTier::summary && getHeight() >= 170 && getWidth() >= 180)
        ModernPanelPainter::paint(g, getLocalBounds(), getWidth() < 320 ? "EQ" : "Parametric EQ",
                                   ModernPanelPainter::Icon::equalizer, 84);
    else
    {
        g.setColour(RendererVisualStyle::panelBackground());
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), RendererVisualStyle::cardRadius());
        g.setColour(RendererVisualStyle::primaryText());
        g.setFont(ModernTypography::font(13.0f, juce::Font::bold));
        g.drawText("EQ", 10, 4, 40, 24, juce::Justification::centredLeft);
    }
    paintCurve(g);
    for (std::size_t index = 0; index < modernBandBounds.size(); ++index)
    {
        const auto cell = modernBandBounds[index];
        if (cell.isEmpty()) continue;
        g.setColour(RendererVisualStyle::borderColour().withAlpha(0.5f));
        g.drawVerticalLine(cell.getRight(), static_cast<float>(cell.getY() + 3), static_cast<float>(cell.getBottom() - 3));
        const auto cellPadding = modernAllBandDetails && cell.getWidth() < 123 ? 1 : 4;
        auto heading = cell.reduced(cellPadding, 0).removeFromTop(std::min(modernAllBandDetails ? 22 : 18,
            bandControls[index].gain.getY() - cell.getY()));
        if (modernAllBandDetails) heading.removeFromLeft(24);
        const auto accent = ModernEqKnobPainter::bandColour(static_cast<int>(index));
        g.setColour(accent.withAlpha(parameters.bands[index].enabled ? 1.0f : 0.55f));
        if (cell.getWidth() < 84)
        {
            g.setFont(ModernTypography::font(10.0f, juce::Font::bold));
            g.setColour(accent.interpolatedWith(RendererVisualStyle::primaryText(), 0.45f));
            g.drawText(compactFrequencyLabel(parameters.bands[index].frequencyHz).replace(" kHz", "k").replace(" Hz", ""),
                       heading, juce::Justification::centred, true);
            continue;
        }
        auto badge = heading.removeFromLeft(14).toFloat().withSizeKeepingCentre(12, 12);
        g.fillEllipse(badge);
        g.setColour(RendererVisualStyle::isNight() ? RendererVisualStyle::panelBackground() : juce::Colours::white);
        g.setFont(ModernTypography::font(9.0f, juce::Font::bold));
        g.drawText(juce::String(static_cast<int>(index + 1)), badge, juce::Justification::centred);
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(ModernTypography::font(10.0f));
        if (heading.getWidth() > 32)
            g.drawText(compactFrequencyLabel(parameters.bands[index].frequencyHz), heading,
                       juce::Justification::centredLeft, true);
    }
    for (const auto& controls : bandControls)
    {
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(ModernTypography::font(10.0f));
        const int unitWidth = modernAllBandDetails ? 16 : 30;
        if (controls.frequency.isVisible())
            g.drawText("Hz", controls.frequency.getBounds().withX(controls.frequency.getX() - unitWidth).withWidth(unitWidth - 2),
                       juce::Justification::centredLeft);
        if (controls.q.isVisible())
            g.drawText("Q", controls.q.getBounds().withX(controls.q.getX() - unitWidth).withWidth(unitWidth - 2),
                       juce::Justification::centredLeft);
        if (controls.gain.isVisible() && controls.gain.getSliderStyle() == juce::Slider::IncDecButtons)
            g.drawText("dB", controls.gain.getBounds().withX(controls.gain.getX() - 30).withWidth(28),
                       juce::Justification::centredLeft);
    }
    if (!modernStatusBounds.isEmpty())
    {
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(ModernTypography::font(10.5f));
        g.drawText(modernDetailsUnavailable ? "Enlarge panel for band details" : statusText,
                   modernStatusBounds, juce::Justification::centredLeft, true);
    }
}

void ParametricEqMagComponent::paintCurve(juce::Graphics& g) const
{
    if (curveBounds.isEmpty())
        return;
    auto area = curveBounds.toFloat();
    if (!RendererVisualStyle::isClassic())
    {
        if (!SkinStyle::isOriginal())
            SkinStyle::paintDataWell(g, area, RendererVisualStyle::componentRadius());
        else
        {
            g.setColour(RendererVisualStyle::panelBackground());
            g.fillRoundedRectangle(area, RendererVisualStyle::componentRadius());
        }
        area = area.withTrimmedLeft(30).withTrimmedRight(8).withTrimmedTop(8).withTrimmedBottom(22);
        if (area.getHeight() < 12 || area.getWidth() < 32) return;
        g.setFont(ModernTypography::font(9.5f));
        const int dbTickStep = area.getHeight() >= 144 ? 6 : area.getHeight() >= 80 ? 12 : 18;
        for (const auto db : { -18, -12, -6, 0, 6, 12, 18 })
        {
            if (db % dbTickStep != 0 || (area.getHeight() < 32 && db != 0)) continue;
            const auto y = area.getCentreY() - static_cast<float>(db) * area.getHeight() / 36.0f;
            g.setColour(RendererVisualStyle::borderColour().withAlpha(db == 0 ? 0.65f : 0.3f));
            g.drawHorizontalLine(juce::roundToInt(y), area.getX(), area.getRight());
            g.setColour(RendererVisualStyle::secondaryText());
            g.drawText(juce::String(db), juce::Rectangle<float>(static_cast<float>(curveBounds.getX()), y - 6, 24, 12), juce::Justification::centredRight);
        }
        auto lastLabelX = area.getX() - 50;
        for (const auto hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            if (hz < curveMinimumHz || hz > curveMaximumHz) continue;
            const auto x = area.getX() + area.getWidth() * static_cast<float>(std::log(hz / curveMinimumHz) / std::log(curveMaximumHz / curveMinimumHz));
            g.setColour(RendererVisualStyle::borderColour().withAlpha(0.3f));
            g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
            if (x - lastLabelX < 42) continue;
            lastLabelX = x;
            g.setColour(RendererVisualStyle::secondaryText());
            g.drawText(hz >= 1000 ? juce::String(hz / 1000, 0) + "k" : juce::String(hz, 0),
                       juce::Rectangle<float>(x - 18, area.getBottom() + 4, 36, 14), juce::Justification::centred);
        }
    }
    else
    {
        g.setColour(RendererVisualStyle::mutedSurface());
        g.fillRoundedRectangle(area, RendererVisualStyle::componentRadius());
        g.setColour(RendererVisualStyle::borderColour().withAlpha(0.7f));
        g.drawRoundedRectangle(area, RendererVisualStyle::componentRadius(), 1.0f);
        for (int line = 1; line < 4; ++line)
        {
            const auto y = area.getY() + area.getHeight() * static_cast<float>(line) / 4.0f;
            g.setColour(RendererVisualStyle::borderColour().withAlpha(line == 2 ? 0.75f : 0.35f));
            g.drawHorizontalLine(juce::roundToInt(y), area.getX() + 4.0f, area.getRight() - 4.0f);
        }
    }

    if (exactMagnitudeDb.size() < 2)
    {
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::captionTextSize())));
        g.drawText(RendererVisualStyle::isClassic() ? "Preview computes the exact coefficient response"
                                                   : "Response unavailable for this source",
                   curveBounds.reduced(10),
                   juce::Justification::centred);
        return;
    }

    const auto makePath = [area] (const std::vector<float>& response)
    {
        juce::Path path;
        if (response.size() < 2) return path;
        for (std::size_t index = 0; index < response.size(); ++index)
        {
            const auto x = area.getX() + area.getWidth()
                         * static_cast<float>(index) / static_cast<float>(response.size() - 1);
            const auto db = juce::jlimit(-18.0f, 18.0f, response[index]);
            const auto y = area.getCentreY() - db * area.getHeight() / 36.0f;
            if (index == 0) path.startNewSubPath(x, y);
            else path.lineTo(x, y);
        }
        return path;
    };
    if (!RendererVisualStyle::isClassic())
    {
        for (std::size_t i = 0; i < bandMagnitudeDb.size(); ++i)
        {
            g.setColour(ModernEqKnobPainter::bandColour(static_cast<int>(i)).withAlpha(0.45f));
            g.strokePath(makePath(bandMagnitudeDb[i]), juce::PathStrokeType(1.0f));
        }
    }
    const auto path = makePath(exactMagnitudeDb);
    // Optional skins keep their accent for actions and draw signal in the signal colour.
    const auto curveColour = SkinStyle::isOriginal() ? RendererVisualStyle::accentBlue() : SkinStyle::signal();
    g.setColour(curveColour);
    if (!RendererVisualStyle::isClassic())
    {
        auto fill = path;
        fill.lineTo(area.getRight(), area.getCentreY());
        fill.lineTo(area.getX(), area.getCentreY());
        fill.closeSubPath();
        g.setColour(curveColour.withAlpha(0.08f));
        g.fillPath(fill);
        g.setColour(curveColour);
    }
    g.strokePath(path, juce::PathStrokeType(2.0f));

    if (!RendererVisualStyle::isClassic() && sourceSampleRate > 0.0
        && curveMaximumHz > curveMinimumHz)
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(curveBounds);
        const auto radius = area.getWidth() < 360 || area.getHeight() < 80 ? 5.5f : 7.0f;
        g.setFont(ModernTypography::font(radius < 7 ? 8.5f : 10.0f));
        // Numbered badges identify the existing controls; they do not add a
        // second, approximate parameter model or a new graph editing gesture.
        for (std::size_t i = 0; i < parameters.bands.size(); ++i)
        {
            const auto& band = parameters.bands[i];
            const auto hz = juce::jlimit(10.0, std::max(10.0, sourceSampleRate * 0.49), band.frequencyHz);
            if (!std::isfinite(hz) || hz < curveMinimumHz || hz > curveMaximumHz) continue;
            const auto proportion = std::log(hz / curveMinimumHz) / std::log(curveMaximumHz / curveMinimumHz);
            const auto sample = proportion * static_cast<double>(exactMagnitudeDb.size() - 1);
            const auto first = std::min(static_cast<std::size_t>(sample), exactMagnitudeDb.size() - 1);
            const auto second = std::min(first + 1, exactMagnitudeDb.size() - 1);
            // Interpolate the displayed total, including a response supplied
            // by Preview, so the marker stays attached to the visible line.
            const auto firstDb = juce::jlimit(-18.0f, 18.0f, exactMagnitudeDb[first]);
            const auto secondDb = juce::jlimit(-18.0f, 18.0f, exactMagnitudeDb[second]);
            const auto db = firstDb + static_cast<float>(sample - static_cast<double>(first)) * (secondDb - firstDb);
            const auto centre = juce::Point<float>(area.getX() + area.getWidth() * static_cast<float>(proportion),
                                                   area.getCentreY() - db * area.getHeight() / 36.0f);
            const auto badge = juce::Rectangle<float>(radius * 2, radius * 2).withCentre(centre);
            g.setColour(band.enabled ? ModernEqKnobPainter::bandColour(static_cast<int>(i))
                                     : RendererVisualStyle::secondaryText().withAlpha(0.7f));
            g.fillEllipse(badge);
            g.setColour(RendererVisualStyle::panelBackground());
            g.drawEllipse(badge, 1.5f);
            g.setColour(RendererVisualStyle::isNight() ? RendererVisualStyle::panelBackground() : juce::Colours::white);
            g.drawText(juce::String(static_cast<int>(i + 1)), badge, juce::Justification::centred);
        }
    }
}

void ParametricEqMagComponent::lookAndFeelChanged()
{
    for (auto& controls : bandControls)
        controls.gain.setColour(juce::Slider::textBoxTextColourId, RendererVisualStyle::primaryText());
}

void ParametricEqMagComponent::resized()
{
    updateVisibilityAndLayout();
}

void ParametricEqMagComponent::updateVisibilityAndLayout()
{
    if (!RendererVisualStyle::isClassic() && informationTier != RendererInformationTier::summary
        && getHeight() >= 170 && getWidth() >= 180)
    {
        updateModernLayout();
        return;
    }
    modernBandBounds.fill({});
    modernStatusBounds = {};
    modernDetailsUnavailable = false;
    modernAllBandDetails = false;
    auto content = getLocalBounds().reduced(informationTier == RendererInformationTier::summary ? 10 : 14);

    const auto summary = informationTier == RendererInformationTier::summary
        || (!RendererVisualStyle::isClassic() && (getHeight() < 170 || getWidth() < 180));
    const auto detail = informationTier == RendererInformationTier::detail;
    const auto showAdvanced = detail && viewMode == ViewMode::advanced;
    const auto showSimple = !summary && viewMode == ViewMode::simple;

    smallButton.setVisible(true);
    mediumButton.setVisible(true);
    largeButton.setVisible(true);
    smallButton.setToggleState(magSize == RendererMagSize::small, juce::dontSendNotification);
    mediumButton.setToggleState(magSize == RendererMagSize::medium, juce::dontSendNotification);
    largeButton.setToggleState(magSize == RendererMagSize::large, juce::dontSendNotification);

    if (summary)
    {
        compactBandGainLayout = true;
        auto compactBounds = getLocalBounds().reduced(6);
        auto sizeRow = compactBounds.removeFromTop(std::min(24, compactBounds.getHeight()));
        if (!RendererVisualStyle::isClassic()) sizeRow.removeFromRight(std::min(26, sizeRow.getWidth()));
        largeButton.setBounds(sizeRow.removeFromRight(std::min(28, sizeRow.getWidth())));
        sizeRow.removeFromRight(std::min(2, sizeRow.getWidth()));
        mediumButton.setBounds(sizeRow.removeFromRight(std::min(28, sizeRow.getWidth())));
        sizeRow.removeFromRight(std::min(2, sizeRow.getWidth()));
        smallButton.setBounds(sizeRow.removeFromRight(std::min(28, sizeRow.getWidth())));
        compactBounds.removeFromTop(std::min(2, compactBounds.getHeight()));

        simpleButton.setVisible(false);
        advancedButton.setVisible(false);
        presetBox.setVisible(false);
        previewButton.setVisible(false);
        applyButton.setVisible(false);
        advancedBandBox.setVisible(false);
        for (std::size_t index = 0; index < simpleLabels.size(); ++index)
        {
            simpleLabels[index].setVisible(false);
            simpleGainSliders[index].setVisible(false);
        }
        for (auto& controls : bandControls)
        {
            controls.enabled.setVisible(false);
            controls.type.setVisible(false);
            controls.frequency.setVisible(false);
            controls.gain.setVisible(true);
            controls.gain.setLookAndFeel(compactKnobLookAndFeel.get());
            controls.gain.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            controls.gain.setTextValueSuffix(" dB");
            controls.gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 14);
            controls.gain.setColour(juce::Slider::textBoxBackgroundColourId,
                                    juce::Colours::transparentBlack);
            controls.gain.setColour(juce::Slider::textBoxOutlineColourId,
                                    juce::Colours::transparentBlack);
            controls.gain.setColour(juce::Slider::textBoxTextColourId,
                                    RendererVisualStyle::primaryText());
            controls.q.setVisible(false);
            controls.slope.setVisible(false);
        }

        const auto portrait = compactBounds.getHeight() > compactBounds.getWidth();
        const auto columns = !RendererVisualStyle::isClassic() && compactBounds.getWidth() >= 288 && compactBounds.getHeight() < 100 ? 8 : portrait ? 2
            : compactBounds.getWidth() >= 520 ? 8
            : compactBounds.getWidth() >= 300 ? 4
                                              : 2;
        const auto rows = static_cast<int>((bandControls.size() + columns - 1) / columns);
        for (std::size_t index = 0; index < bandControls.size(); ++index)
        {
            const auto column = static_cast<int>(index) % columns;
            const auto row = static_cast<int>(index) / columns;
            const auto x0 = compactBounds.getX() + (compactBounds.getWidth() * column) / columns;
            const auto x1 = compactBounds.getX() + (compactBounds.getWidth() * (column + 1)) / columns;
            const auto y0 = compactBounds.getY() + (compactBounds.getHeight() * row) / rows;
            const auto y1 = compactBounds.getY() + (compactBounds.getHeight() * (row + 1)) / rows;
            auto cell = juce::Rectangle<int>::leftTopRightBottom(x0, y0, x1, y1).reduced(2);
            const auto labelReserve = std::min(12, std::max(2, cell.getHeight() / 3));
            cell.removeFromTop(std::min(labelReserve, cell.getHeight() - 1));
            if (!RendererVisualStyle::isClassic())
            {
                auto& gain = bandControls[index].gain;
                const bool showValue = cell.getHeight() >= 38;
                gain.setTextBoxStyle(showValue ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox,
                                     false, 50, showValue ? 14 : 0);
                gain.getProperties().set("modernKnobDiameter", juce::jlimit(28.0, 38.0, getWidth() * 0.036));
                gain.setTooltip("Band " + juce::String(static_cast<int>(index + 1)) + " gain (dB)");
            }
            bandControls[index].gain.setBounds(cell);
            modernBandBounds[index] = juce::Rectangle<int>(x0, y0, x1 - x0, y1 - y0);
        }
        curveBounds = {};
        return;
    }

    compactBandGainLayout = false;

    content.removeFromTop(46);
    auto header = content.removeFromTop(28);
    largeButton.setBounds(header.removeFromRight(36));
    header.removeFromRight(4);
    mediumButton.setBounds(header.removeFromRight(36));
    header.removeFromRight(4);
    smallButton.setBounds(header.removeFromRight(36));
    header.removeFromRight(8);

    simpleButton.setVisible(!summary);
    advancedButton.setVisible(!summary);
    if (!summary)
    {
        simpleButton.setBounds(header.removeFromLeft(66));
        header.removeFromLeft(4);
        advancedButton.setBounds(header.removeFromLeft(82));
        header.removeFromLeft(8);
    }
    simpleButton.setToggleState(viewMode == ViewMode::simple, juce::dontSendNotification);
    advancedButton.setToggleState(viewMode == ViewMode::advanced, juce::dontSendNotification);

    presetBox.setVisible(!summary);
    if (!summary)
        presetBox.setBounds(header);

    auto actionRow = content.removeFromBottom(30);
    applyButton.setVisible(true);
    previewButton.setVisible(true);
    applyButton.setBounds(actionRow.removeFromRight(72));
    actionRow.removeFromRight(6);
    previewButton.setBounds(actionRow.removeFromRight(summary ? 92 : 112));

    for (std::size_t index = 0; index < simpleLabels.size(); ++index)
    {
        simpleLabels[index].setVisible(showSimple);
        simpleGainSliders[index].setVisible(showSimple);
    }
    for (auto& controls : bandControls)
    {
        controls.enabled.setVisible(false);
        controls.type.setVisible(false);
        controls.frequency.setVisible(false);
        controls.gain.setVisible(false);
        controls.gain.setLookAndFeel(nullptr);
        controls.gain.setSliderStyle(juce::Slider::LinearHorizontal);
        controls.gain.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 22);
        controls.q.setVisible(false);
        controls.slope.setVisible(false);
    }
    advancedBandBox.setVisible(showAdvanced);

    curveBounds = {};

    content.removeFromTop(8);
    if (showSimple)
    {
        if (detail && content.getHeight() > 210)
        {
            curveBounds = content.removeFromTop(std::min(126, content.getHeight() / 2));
            content.removeFromTop(8);
        }
        const auto rowHeight = std::max(26, std::min(40, content.getHeight() / 3));
        for (std::size_t index = 0; index < simpleLabels.size(); ++index)
        {
            auto row = content.removeFromTop(std::min(rowHeight, content.getHeight()));
            simpleLabels[index].setBounds(row.removeFromLeft(76));
            simpleGainSliders[index].setBounds(row);
        }
        return;
    }

    if (!showAdvanced)
        return;

    curveBounds = content.removeFromTop(std::min(64, std::max(44, content.getHeight() / 3)));
    content.removeFromTop(std::min(5, content.getHeight()));
    selectedAdvancedBand = std::min(selectedAdvancedBand, bandControls.size() - 1);
    auto& controls = bandControls[selectedAdvancedBand];
    controls.enabled.setVisible(true);
    controls.type.setVisible(true);
    controls.frequency.setVisible(true);
    controls.gain.setVisible(true);
    controls.q.setVisible(true);
    controls.slope.setVisible(true);

    auto selectorRow = content.removeFromTop(std::min(26, content.getHeight()));
    advancedBandBox.setBounds(selectorRow.removeFromLeft(std::min(92, selectorRow.getWidth())));
    selectorRow.removeFromLeft(std::min(4, selectorRow.getWidth()));
    controls.enabled.setBounds(selectorRow.removeFromLeft(std::min(46, selectorRow.getWidth())));
    selectorRow.removeFromLeft(std::min(4, selectorRow.getWidth()));
    controls.type.setBounds(selectorRow);
    content.removeFromTop(std::min(4, content.getHeight()));

    const auto rowGap = std::min(4, content.getHeight());
    const auto controlRowHeight = std::max(22, (content.getHeight() - rowGap) / 2);
    auto primaryRow = content.removeFromTop(std::min(controlRowHeight, content.getHeight()));
    auto frequencyBounds = primaryRow.removeFromLeft(std::max(0, (primaryRow.getWidth() - 4) / 2));
    primaryRow.removeFromLeft(std::min(4, primaryRow.getWidth()));
    controls.frequency.setBounds(frequencyBounds);
    controls.gain.setBounds(primaryRow);
    content.removeFromTop(std::min(rowGap, content.getHeight()));

    auto finalRow = content.removeFromTop(std::min(controlRowHeight, content.getHeight()));
    controls.slope.setBounds(finalRow.removeFromRight(std::min(82, finalRow.getWidth() / 3)));
    finalRow.removeFromRight(std::min(4, finalRow.getWidth()));
    controls.q.setBounds(finalRow);
}

void ParametricEqMagComponent::updateModernLayout()
{
    getProperties().set("workspace.headerHeight", ModernPanelPainter::headerHeight);
    auto content = getLocalBounds().withTrimmedTop(ModernPanelPainter::headerHeight).reduced(8, 6);
    modernBandBounds.fill({});
    modernStatusBounds = {};
    modernDetailsUnavailable = false;
    curveBounds = {};
    compactBandGainLayout = true;
    const bool wantsAdvanced = viewMode == ViewMode::advanced;
    for (auto* button : { &smallButton, &mediumButton, &largeButton, &simpleButton, &advancedButton })
        button->setVisible(true);
    smallButton.setToggleState(magSize == RendererMagSize::small, juce::dontSendNotification);
    mediumButton.setToggleState(magSize == RendererMagSize::medium, juce::dontSendNotification);
    largeButton.setToggleState(magSize == RendererMagSize::large, juce::dontSendNotification);
    simpleButton.setToggleState(viewMode == ViewMode::simple, juce::dontSendNotification);
    advancedButton.setToggleState(viewMode == ViewMode::advanced, juce::dontSendNotification);
    auto header = getLocalBounds().removeFromTop(32).reduced(6, 4).withTrimmedRight(26);
    for (auto* button : { &largeButton, &mediumButton, &smallButton })
    {
        button->setBounds(header.removeFromRight(std::min(26, header.getWidth())));
        header.removeFromRight(std::min(2, header.getWidth()));
    }
    auto toolbar = content.removeFromTop(std::min(26, content.getHeight()));
    auto modes = toolbar.removeFromLeft(std::min(152, toolbar.getWidth()));
    simpleButton.setBounds(modes.removeFromLeft(std::min(62, modes.getWidth())));
    modes.removeFromLeft(std::min(4, modes.getWidth()));
    advancedButton.setBounds(modes.removeFromLeft(std::min(82, modes.getWidth())));
    const bool actionsInToolbar = content.getWidth() >= 640;
    applyButton.setVisible(true); previewButton.setVisible(true);
    if (actionsInToolbar)
    {
        applyButton.setBounds(toolbar.removeFromRight(60));
        toolbar.removeFromRight(5);
        previewButton.setBounds(toolbar.removeFromRight(94));
        toolbar.removeFromRight(8);
    }
    presetBox.setVisible(toolbar.getWidth() >= 64);
    auto presetBounds = toolbar.reduced(4, 0);
    presetBounds.setWidth(juce::jmin(300, presetBounds.getWidth()));
    presetBox.setBounds(presetBox.isVisible() ? presetBounds : juce::Rectangle<int>());
    content.removeFromTop(std::min(4, content.getHeight()));

    auto footer = content.removeFromBottom(std::min(actionsInToolbar ? 14 : 26, content.getHeight()));
    if (!actionsInToolbar)
    {
        applyButton.setBounds(footer.removeFromRight(std::min(60, footer.getWidth())));
        footer.removeFromRight(std::min(5, footer.getWidth()));
        previewButton.setBounds(footer.removeFromRight(std::min(94, footer.getWidth())));
    }
    modernStatusBounds = footer.reduced(2, 0);
    content.removeFromBottom(std::min(4, content.getHeight()));

    // Eight 115px cells still fit signed dB, six-digit Hz and the existing
    // increment controls. Prefer the full row in the ordinary wide DSP Mag;
    // below that width retain the complete selected-band inspector.
    const bool allBandDetails = wantsAdvanced && informationTier == RendererInformationTier::detail
        && content.getWidth() >= 8 * 115 && content.getHeight() >= 240;
    modernAllBandDetails = allBandDetails;
    for (auto& label : simpleLabels) label.setVisible(false);
    for (auto& slider : simpleGainSliders) slider.setVisible(false);
    for (auto& controls : bandControls)
    {
        for (auto* child : std::initializer_list<juce::Component*> { &controls.enabled, &controls.type,
                 &controls.frequency, &controls.q, &controls.slope }) child->setVisible(false);
        controls.frequency.getProperties().set("modernCompactNumeric", false);
        controls.q.getProperties().set("modernCompactNumeric", false);
        controls.frequency.setSliderStyle(juce::Slider::LinearHorizontal);
        controls.q.setSliderStyle(juce::Slider::LinearHorizontal);
        controls.q.setTextValueSuffix("");
        controls.gain.setVisible(true);
        controls.gain.getProperties().set("modernInlineGain", allBandDetails);
        controls.gain.setLookAndFeel(compactKnobLookAndFeel.get());
        controls.gain.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        controls.gain.setTextValueSuffix(" dB");
        controls.gain.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        controls.gain.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        controls.gain.setColour(juce::Slider::textBoxTextColourId, RendererVisualStyle::primaryText());
        controls.gain.getProperties().set("modernKnobDiameter", juce::jlimit(28.0, 38.0, getWidth() * 0.036));
    }
    const bool wideInspector = content.getWidth() >= 360;
    const int inspectorHeight = wideInspector ? 60 : 124;
    const bool selectedBandDetails = wantsAdvanced && !allBandDetails
        && content.getWidth() >= 180 && content.getHeight() >= inspectorHeight;
    modernDetailsUnavailable = wantsAdvanced && !allBandDetails && !selectedBandDetails;
    advancedBandBox.setVisible(selectedBandDetails);
    if (selectedBandDetails)
    {
        // Prefer a complete selected-band inspector to eight knobs that leave
        // the actual Advanced parameters inaccessible in a short native Mag.
        const auto remainingHeight = content.getHeight() - inspectorHeight - 6;
        const int overviewColumns = content.getWidth() >= 704
            || (content.getWidth() >= 400 && remainingHeight < 204) ? 8
            : content.getWidth() >= 320 ? 4 : 2;
        const bool showOverview = remainingHeight >= (8 / overviewColumns) * 70;
        auto detail = content.removeFromBottom(inspectorHeight);
        auto row = detail.removeFromTop(28);
        advancedBandBox.setBounds(row.removeFromLeft(76));
        row.removeFromLeft(4);
        selectedAdvancedBand = std::min(selectedAdvancedBand, bandControls.size() - 1);
        auto& controls = bandControls[selectedAdvancedBand];
        controls.enabled.setButtonText("");
        for (auto* child : std::initializer_list<juce::Component*> { &controls.enabled, &controls.type,
                 &controls.frequency, &controls.q, &controls.slope }) child->setVisible(true);
        controls.enabled.setBounds(row.removeFromLeft(24));
        row.removeFromLeft(4);
        if (wideInspector)
        {
            controls.slope.setBounds(row.removeFromRight(100));
            row.removeFromRight(4);
        }
        controls.type.setBounds(row);

        auto placeNumber = [] (juce::Slider& slider, juce::Rectangle<int> bounds)
        {
            bounds = bounds.withTrimmedLeft(30);
            // Inherit the live theme. Explicit Light colours would survive a
            // theme switch after this editor has already been laid out.
            slider.removeColour(juce::Slider::textBoxBackgroundColourId);
            slider.removeColour(juce::Slider::textBoxOutlineColourId);
            slider.setSliderStyle(juce::Slider::IncDecButtons);
            slider.setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_AutoDirection);
            slider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, std::max(24, bounds.getWidth() - 32), 24);
            slider.setBounds(bounds);
        };
        detail.removeFromTop(4);
        row = detail.removeFromTop(28);
        if (wideInspector)
        {
            const auto fieldWidth = row.getWidth() / (showOverview ? 2 : 3);
            placeNumber(controls.frequency, row.removeFromLeft(fieldWidth).withTrimmedRight(4));
            placeNumber(controls.q, showOverview ? row : row.removeFromLeft(fieldWidth).withTrimmedRight(4));
            if (!showOverview) placeNumber(controls.gain, row);
        }
        else
        {
            placeNumber(controls.frequency, row);
            detail.removeFromTop(4);
            row = detail.removeFromTop(28);
            if (!showOverview)
                placeNumber(controls.gain, row.removeFromRight(row.getWidth() / 2));
            placeNumber(controls.q, row.withTrimmedRight(showOverview ? 0 : 4));
            detail.removeFromTop(4);
            controls.slope.setBounds(detail.removeFromTop(28));
        }
        content.removeFromBottom(std::min(6, content.getHeight()));
        if (!showOverview)
        {
            for (std::size_t index = 0; index < bandControls.size(); ++index)
                bandControls[index].gain.setVisible(index == selectedAdvancedBand);
            controls.gain.setTextValueSuffix("");
            controls.gain.setLookAndFeel(nullptr);
            if (content.getHeight() > 48) curveBounds = content.withTrimmedBottom(6);
            return;
        }
    }

    const bool shortWideBands = content.getWidth() >= 400 && content.getHeight() < (selectedBandDetails ? 204 : 140);
    const auto columns = content.getWidth() >= 704 || shortWideBands ? 8 : content.getWidth() >= 320 ? 4 : 2;
    const auto rows = 8 / columns;
    const auto bandHeight = std::min(content.getHeight(), allBandDetails ? 112 : rows * 78);
    auto bands = content.removeFromBottom(bandHeight);
    if (content.getHeight() > 48) curveBounds = content.withTrimmedBottom(6);
    for (int i = 0; i < 8; ++i)
    {
        auto cell = juce::Rectangle<int>::leftTopRightBottom(
            bands.getX() + bands.getWidth() * (i % columns) / columns,
            bands.getY() + bands.getHeight() * (i / columns) / rows,
            bands.getX() + bands.getWidth() * (i % columns + 1) / columns,
            bands.getY() + bands.getHeight() * (i / columns + 1) / rows);
        modernBandBounds[static_cast<size_t>(i)] = cell;
        auto& controls = bandControls[static_cast<size_t>(i)];
        auto& gain = controls.gain;
        if (!allBandDetails) cell.removeFromTop(std::min(18, cell.getHeight()));
        gain.setTooltip("Band " + juce::String(i + 1) + " | "
            + juce::String(juce::roundToInt(parameters.bands[static_cast<size_t>(i)].frequencyHz)) + " Hz | Gain (dB)");
        if (allBandDetails)
        {
            const bool denseDetails = cell.getWidth() < 123;
            cell = cell.reduced(denseDetails ? 1 : 4, 0);
            auto heading = cell.removeFromTop(22);
            controls.enabled.setVisible(true);
            controls.enabled.setButtonText("");
            controls.enabled.setBounds(heading.removeFromLeft(22));
            auto parametersRow = cell.removeFromTop(56);
            gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 40, 14);
            gain.setBounds(parametersRow.removeFromLeft(40));
            parametersRow.removeFromLeft(denseDetails ? 2 : 4);
            for (auto* numeric : { &controls.frequency, &controls.q })
            {
                auto numericRow = parametersRow.removeFromTop(24).withTrimmedLeft(16);
                numeric->setVisible(true);
                numeric->getProperties().set("modernCompactNumeric", true);
                numeric->setSliderStyle(juce::Slider::IncDecButtons);
                numeric->setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_AutoDirection);
                numeric->setTextBoxStyle(juce::Slider::TextBoxLeft, false, numericRow.getWidth() - 16, 22);
                numeric->setBounds(numericRow);
                parametersRow.removeFromTop(3);
            }
            cell.removeFromTop(2);
            auto filters = cell.removeFromTop(24);
            controls.slope.setVisible(true);
            controls.slope.setBounds(filters.removeFromRight(58));
            filters.removeFromRight(4);
            controls.type.setVisible(true);
            controls.type.setBounds(filters);
            controls.type.setTooltip("Band " + juce::String(i + 1) + " filter: "
                + filterTypeName(parameters.bands[static_cast<size_t>(i)].type));
            continue;
        }
        gain.setTextBoxStyle(cell.getHeight() >= 34 ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox,
                             false, std::min(60, cell.getWidth()), cell.getHeight() >= 34 ? 14 : 0);
        gain.setBounds(cell.reduced(2, 0));
    }
}

bool ParametricEqMagComponent::areSizeControlsUsableForTesting() const noexcept
{
    const auto bounds = getLocalBounds();
    return smallButton.isVisible() && mediumButton.isVisible() && largeButton.isVisible()
        && !smallButton.getBounds().isEmpty()
        && bounds.contains(smallButton.getBounds())
        && bounds.contains(mediumButton.getBounds())
        && bounds.contains(largeButton.getBounds());
}

void ParametricEqMagComponent::updateControlsFromModel()
{
    synchronisingControls = true;
    const auto presets = ParametricEqProcessor::builtInPresets();
    const auto preset = std::find_if(presets.begin(), presets.end(), [this] (const auto& item)
    {
        return item.stableId == parameters.presetId;
    });
    presetBox.setSelectedId(preset == presets.end()
                                ? 0
                                : static_cast<int>(std::distance(presets.begin(), preset)) + 1,
                            juce::dontSendNotification);

    for (std::size_t index = 0; index < bandControls.size(); ++index)
    {
        const auto& band = parameters.bands[index];
        auto& controls = bandControls[index];
        controls.enabled.setToggleState(band.enabled, juce::dontSendNotification);
        controls.type.setSelectedId(idForFilterType(band.type), juce::dontSendNotification);
        controls.frequency.setValue(band.frequencyHz, juce::dontSendNotification);
        controls.gain.setValue(band.gainDb, juce::dontSendNotification);
        controls.q.setValue(band.q, juce::dontSendNotification);
        controls.slope.setSelectedId(band.slope == ParametricEqSlope::db24 ? 2 : 1,
                                     juce::dontSendNotification);
    }
    for (std::size_t index = 0; index < simpleGainSliders.size(); ++index)
        simpleGainSliders[index].setValue(parameters.bands[simpleBandIndices[index]].gainDb,
                                          juce::dontSendNotification);
    synchronisingControls = false;
    refreshCoefficientCurve();
}

void ParametricEqMagComponent::emitParametersChanged()
{
    parameters.presetId = "custom";
    presetBox.setSelectedId(0, juce::dontSendNotification);
    refreshCoefficientCurve();
    if (onParametersChanged)
        onParametersChanged(parameters);
    repaint();
}

void ParametricEqMagComponent::handlePresetChanged()
{
    if (synchronisingControls || presetBox.getSelectedId() <= 0)
        return;
    const auto presets = ParametricEqProcessor::builtInPresets();
    const auto index = static_cast<std::size_t>(presetBox.getSelectedId() - 1);
    if (index >= presets.size())
        return;
    parameters = presets[index].parameters;
    updateControlsFromModel();
    if (onParametersChanged)
        onParametersChanged(parameters);
    repaint();
}

void ParametricEqMagComponent::handleBandControlChanged(std::size_t index)
{
    if (synchronisingControls || index >= bandControls.size())
        return;
    auto& band = parameters.bands[index];
    const auto before = band;
    const auto& controls = bandControls[index];
    band.enabled = controls.enabled.getToggleState();
    band.type = filterTypeForId(controls.type.getSelectedId());
    band.slope = controls.slope.getSelectedId() == 2 ? ParametricEqSlope::db24
                                                     : ParametricEqSlope::db12;
    if (band != before) emitParametersChanged();
}

void ParametricEqMagComponent::handleNumericValueChanged(std::size_t index, NumericField field,
                                                        double value, bool forceEnabled)
{
    if (synchronisingControls || index >= bandControls.size()) return;
    auto& band = parameters.bands[index];
    const auto before = band;
    switch (field)
    {
        case NumericField::frequency: band.frequencyHz = value; break;
        case NumericField::gain:
            band.gainDb = value;
            if (compactBandGainLayout || forceEnabled) band.enabled = true;
            break;
        case NumericField::q: band.q = value; break;
    }
    if (band == before) return;
    bandControls[index].enabled.setToggleState(band.enabled, juce::dontSendNotification);
    // During live text input the owning JUCE Slider keeps its committed value;
    // changing it would close the editor. Synchronise once editing ends.
    if (!numericEditStart.has_value() && field == NumericField::gain)
    {
        bandControls[index].gain.setValue(value, juce::dontSendNotification);
        for (std::size_t i = 0; i < simpleBandIndices.size(); ++i)
            if (simpleBandIndices[i] == index) simpleGainSliders[i].setValue(value, juce::dontSendNotification);
    }
    emitParametersChanged();
}

void ParametricEqMagComponent::handleSimpleGainChanged(std::size_t controlIndex)
{
    if (controlIndex < simpleBandIndices.size())
        handleNumericValueChanged(simpleBandIndices[controlIndex], NumericField::gain,
                                   simpleGainSliders[controlIndex].getValue(), true);
}

void ParametricEqMagComponent::requestMagSize(RendererMagSize size)
{
    magSize = size;
    if (onMagSizeRequested)
        onMagSizeRequested(size);
    updateVisibilityAndLayout();
    repaint();
}

ParametricEqFilterType ParametricEqMagComponent::filterTypeForId(int id) noexcept
{
    switch (id)
    {
        case 1: return ParametricEqFilterType::highPass;
        case 2: return ParametricEqFilterType::lowShelf;
        case 3: return ParametricEqFilterType::bell;
        case 4: return ParametricEqFilterType::notch;
        case 5: return ParametricEqFilterType::highShelf;
        case 6: return ParametricEqFilterType::lowPass;
        default: return ParametricEqFilterType::bell;
    }
}

int ParametricEqMagComponent::idForFilterType(ParametricEqFilterType type) noexcept
{
    switch (type)
    {
        case ParametricEqFilterType::highPass: return 1;
        case ParametricEqFilterType::lowShelf: return 2;
        case ParametricEqFilterType::bell: return 3;
        case ParametricEqFilterType::notch: return 4;
        case ParametricEqFilterType::highShelf: return 5;
        case ParametricEqFilterType::lowPass: return 6;
    }
    return 3;
}

void ParametricEqMagComponent::setSimpleGainForTesting(int controlIndex, double gainDb)
{
    if (!juce::isPositiveAndBelow(controlIndex, static_cast<int>(simpleGainSliders.size())))
        return;
    simpleGainSliders[static_cast<std::size_t>(controlIndex)].setValue(gainDb,
                                                                      juce::sendNotificationSync);
}

void ParametricEqMagComponent::setCompactBandGainForTesting(int bandIndex, double gainDb)
{
    if (!juce::isPositiveAndBelow(bandIndex, static_cast<int>(bandControls.size())))
        return;
    bandControls[static_cast<std::size_t>(bandIndex)].gain.setValue(gainDb,
                                                                    juce::sendNotificationSync);
}

bool ParametricEqMagComponent::isBandControlVisibleForTesting(int bandIndex) const
{
    return juce::isPositiveAndBelow(bandIndex, static_cast<int>(bandControls.size()))
        && bandControls[static_cast<std::size_t>(bandIndex)].enabled.isVisible();
}

int ParametricEqMagComponent::visibleCompactBandGainCountForTesting() const noexcept
{
    return static_cast<int>(std::count_if(bandControls.begin(), bandControls.end(), [] (const auto& controls)
    {
        return controls.gain.isVisible() && !controls.gain.getBounds().isEmpty();
    }));
}

bool ParametricEqMagComponent::compactBandGainsFitBoundsForTesting() const noexcept
{
    const auto bounds = getLocalBounds();
    return std::all_of(bandControls.begin(), bandControls.end(), [&bounds] (const auto& controls)
    {
        return controls.gain.isVisible() && !controls.gain.getBounds().isEmpty()
            && bounds.contains(controls.gain.getBounds());
    });
}
} // namespace singleeditor
