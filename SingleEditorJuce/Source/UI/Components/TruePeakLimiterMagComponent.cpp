#include "UI/Components/TruePeakLimiterMagComponent.h"

#include <cmath>

#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernPanelPainter.h"

namespace singleeditor
{
TruePeakLimiterMagComponent::TruePeakLimiterMagComponent(
    TruePeakLimiterParameters initialParameters)
    : parameters(std::move(initialParameters))
    , statusText(parameters.enabled ? "Ready | Preview is audition-only" : "Off | zero processing")
{
    setInterceptsMouseClicks(true, true);
    setWantsKeyboardFocus(true);
    setDescription("Linked lookahead AudioSuite true-peak limiter. Off performs no processing; Preview is temporary and Apply creates one verified undoable edit.");

    enabledButton.setComponentID("limiter.enabled");
    enabledButton.setName("True-Peak Limiter enabled");
    enabledButton.setTitle("Enable or disable the True-Peak Limiter");
    enabledButton.setClickingTogglesState(true);
    enabledButton.getProperties().set("modernCompactSwitch", true);
    enabledButton.onClick = [this]
    {
        if (synchronisingControls)
            return;
        parameters.enabled = enabledButton.getToggleState();
        emitParametersChanged();
    };
    addAndMakeVisible(enabledButton);

    configureButton(smallButton, "S", "limiter.size.small");
    configureButton(mediumButton, "M", "limiter.size.medium");
    configureButton(largeButton, "L", "limiter.size.large");
    configureButton(recallButton, "Last Applied", "limiter.recall-last-applied");
    configureButton(previewButton, "Preview", "limiter.preview");
    configureButton(applyButton, "Apply", "limiter.apply");
    smallButton.onClick = [this] { requestMagSize(RendererMagSize::small); };
    mediumButton.onClick = [this] { requestMagSize(RendererMagSize::medium); };
    largeButton.onClick = [this] { requestMagSize(RendererMagSize::large); };
    recallButton.onClick = [this]
    {
        if (!applying && recallAvailable && onRecallLastAppliedRequested)
            onRecallLastAppliedRequested();
    };
    previewButton.onClick = [this]
    {
        if (!applying && parameters.enabled && onPreviewRequested)
            onPreviewRequested(!previewing);
    };
    applyButton.onClick = [this]
    {
        if (!applying && parameters.enabled && onApplyRequested)
            onApplyRequested();
    };

    thresholdLabel.setText("THRESHOLD", juce::dontSendNotification);
    ceilingLabel.setText("CEILING", juce::dontSendNotification);
    releaseLabel.setText("RELEASE", juce::dontSendNotification);
    thresholdLabel.setComponentID("limiter.threshold-label");
    ceilingLabel.setComponentID("limiter.ceiling-label");
    releaseLabel.setComponentID("limiter.release-label");
    for (auto* label : { &thresholdLabel, &ceilingLabel, &releaseLabel })
    {
        ModernAppleLookAndFeel::setLabelRole(*label, ModernAppleLookAndFeel::LabelRole::secondary);
        label->setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(*label);
    }

    configureSlider(thresholdSlider, -36.0, 0.0, 0.1, " dB", "Limiter threshold");
    thresholdSlider.setComponentID("limiter.threshold-db");
    thresholdSlider.onValueChange = [this]
    {
        if (!synchronisingControls)
        {
            parameters.thresholdDb = thresholdSlider.getValue();
            emitParametersChanged();
        }
    };
    configureSlider(ceilingSlider, -12.0, 0.0, 0.1, " dBTP", "Limiter ceiling");
    ceilingSlider.setComponentID("limiter.ceiling-dbtp");
    ceilingSlider.onValueChange = [this]
    {
        if (!synchronisingControls)
        {
            parameters.ceilingDbtp = ceilingSlider.getValue();
            emitParametersChanged();
        }
    };
    configureSlider(releaseSlider, 20.0, 2000.0, 1.0, " ms", "Limiter manual release");
    releaseSlider.setSkewFactorFromMidPoint(180.0);
    releaseSlider.setComponentID("limiter.release-ms");
    releaseSlider.onValueChange = [this]
    {
        if (!synchronisingControls)
        {
            parameters.releaseMs = releaseSlider.getValue();
            emitParametersChanged();
        }
    };

    releaseModeBox.setComponentID("limiter.release-mode");
    releaseModeBox.setName("Limiter release mode");
    releaseModeBox.addItem("Auto", 1);
    releaseModeBox.addItem("Manual", 2);
    releaseModeBox.onChange = [this]
    {
        if (!synchronisingControls)
        {
            parameters.releaseMode = releaseModeBox.getSelectedId() == 2
                ? TruePeakLimiterReleaseMode::manual
                : TruePeakLimiterReleaseMode::automatic;
            emitParametersChanged();
        }
    };
    addAndMakeVisible(releaseModeBox);

    lockedFactsLabel.setComponentID("limiter.locked-profile");
    lockedFactsLabel.setName("Locked limiter profile");
    lockedFactsLabel.setText("5 ms LOOKAHEAD  |  RATE-AWARE TP  |  LINKED CHANNELS  |  0.10 dB GUARD",
                             juce::dontSendNotification);
    ModernAppleLookAndFeel::setLabelRole(lockedFactsLabel, ModernAppleLookAndFeel::LabelRole::secondary);
    lockedFactsLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(lockedFactsLabel);

    updateControlsFromModel();
    updateVisibilityAndLayout();
}

void TruePeakLimiterMagComponent::configureButton(juce::TextButton& button,
                                                   const juce::String& text,
                                                   const juce::String& componentId)
{
    button.setButtonText(text);
    button.setComponentID(componentId);
    button.setName(text);
    button.setTitle(text);
    addAndMakeVisible(button);
}

void TruePeakLimiterMagComponent::configureSlider(juce::Slider& slider,
                                                   double minimum,
                                                   double maximum,
                                                   double interval,
                                                   const juce::String& suffix,
                                                   const juce::String& name)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 82, 22);
    slider.setRange(minimum, maximum, interval);
    slider.setTextValueSuffix(suffix);
    slider.setName(name);
    slider.setTitle(name);
    slider.getProperties().set("modernParameterSlider", true);
    addAndMakeVisible(slider);
}

void TruePeakLimiterMagComponent::setParameters(const TruePeakLimiterParameters& next)
{
    parameters = next;
    updateControlsFromModel();
    updateVisibilityAndLayout();
    repaint();
}

const TruePeakLimiterParameters& TruePeakLimiterMagComponent::getParameters() const noexcept
{
    return parameters;
}

void TruePeakLimiterMagComponent::setPreviewing(bool shouldPreview)
{
    previewing = shouldPreview;
    previewButton.setButtonText(previewing ? "Stop Preview" : "Preview");
    previewButton.setName(previewing ? "Stop Preview" : "Preview");
    previewButton.setTitle(previewing ? "Stop True-Peak Limiter Preview"
                                     : "Start True-Peak Limiter Preview");
    previewButton.setToggleState(previewing, juce::dontSendNotification);
    updateVisibilityAndLayout();
    repaint();
}

void TruePeakLimiterMagComponent::setApplying(bool shouldApply)
{
    applying = shouldApply;
    applyButton.setButtonText(applying ? "Applying..." : "Apply");
    updateVisibilityAndLayout();
    repaint();
}

void TruePeakLimiterMagComponent::setRecallAvailable(bool available)
{
    recallAvailable = available;
    updateVisibilityAndLayout();
}

void TruePeakLimiterMagComponent::setStatusText(juce::String status)
{
    statusText = std::move(status);
    repaint();
}

void TruePeakLimiterMagComponent::setFeedback(TruePeakLimiterFeedbackSnapshot snapshot)
{
    feedback = snapshot;
    feedbackVisible = true;
    if (feedback.processedSamples > 0)
    {
        statusText = juce::String(applying ? "Apply" : (previewing ? "Preview" : "Feedback"))
                   + " | TP " + formatDb(feedback.inputTruePeakDbtp, " dBTP")
                   + " | GR " + formatDb(feedback.maximumGainReductionDb, " dB");
    }
    repaint();
}

void TruePeakLimiterMagComponent::clearFeedback()
{
    feedback = {};
    feedbackVisible = false;
    repaint();
}

void TruePeakLimiterMagComponent::applyWorkspaceVisualContract(
    RendererDensity nextDensity,
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

void TruePeakLimiterMagComponent::paint(juce::Graphics& g)
{
    if (!RendererVisualStyle::isClassic())
    {
        ModernPanelPainter::paint(g, getLocalBounds(), "Limiter", ModernPanelPainter::Icon::meter);
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(ModernTypography::font(10.5f));
        g.drawText(statusText, modernStatusBounds, juce::Justification::centredLeft, true);
        paintModernTransferCurve(g);
        return;
    }
    else
    {
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(RendererVisualStyle::panelBackground());
    g.fillRoundedRectangle(bounds.reduced(0.5f), RendererVisualStyle::cardRadius());

    auto content = getLocalBounds().reduced(informationTier == RendererInformationTier::summary ? 10 : 14);
    g.setColour(RendererVisualStyle::primaryText());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::titleTextSize(), juce::Font::bold)));
    g.drawText(informationTier == RendererInformationTier::summary ? "LIMIT" : "True-Peak Limiter",
               content.removeFromTop(24),
               juce::Justification::centredLeft);

    g.setColour(RendererVisualStyle::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::captionTextSize())));
    auto status = statusText;
    if (!sweetSpot)
        status += " | Resize for full controls";
    g.drawText(status, content.removeFromTop(18), juce::Justification::centredLeft, true);

    if (informationTier == RendererInformationTier::summary)
    {
        g.setColour(parameters.enabled ? RendererVisualStyle::accentInk()
                                       : RendererVisualStyle::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::monoTextSize(), juce::Font::bold)));
        g.drawText(parameters.enabled
                       ? formatDb(parameters.ceilingDbtp, " dBTP CEILING")
                       : "OFF  |  ZERO PROCESSING",
                   content.withTrimmedTop(42).removeFromTop(24),
                   juce::Justification::centredLeft);
    }

    }
    if (feedbackBounds.isEmpty())
        return;

    const auto area = feedbackBounds.toFloat();
    g.setColour(RendererVisualStyle::mutedSurface());
    g.fillRoundedRectangle(area, RendererVisualStyle::componentRadius());
    g.setColour(RendererVisualStyle::borderColour().withAlpha(0.72f));
    g.drawRoundedRectangle(area, RendererVisualStyle::componentRadius(), 1.0f);
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::monoTextSize(), juce::Font::bold)));
    if (!feedbackVisible || feedback.processedSamples <= 0)
    {
        g.setColour(RendererVisualStyle::secondaryText());
        g.drawText(feedbackVisible ? "WAITING FOR PROCESSED SAMPLES"
                                   : "TP / GR APPEAR ONLY DURING PREVIEW OR APPLY",
                   feedbackBounds.reduced(10),
                   juce::Justification::centred,
                   true);
        return;
    }

    auto left = feedbackBounds.reduced(12);
    auto right = left.removeFromRight(left.getWidth() / 2);
    g.setColour(RendererVisualStyle::accentBlue());
    g.drawText(formatDb(feedback.inputTruePeakDbtp, " dBTP"),
               left.removeFromTop(24), juce::Justification::centredLeft);
    g.setColour(RendererVisualStyle::secondaryText());
    g.drawText("INPUT TRUE PEAK", left.removeFromTop(18), juce::Justification::centredLeft);
    g.setColour(RendererVisualStyle::accentOrange());
    g.drawText(formatDb(feedback.maximumGainReductionDb, " dB"),
               right.removeFromTop(24), juce::Justification::centredLeft);
    g.setColour(RendererVisualStyle::secondaryText());
    g.drawText("MAX GAIN REDUCTION", right.removeFromTop(18), juce::Justification::centredLeft);
}

void TruePeakLimiterMagComponent::resized()
{
    updateVisibilityAndLayout();
}

double TruePeakLimiterMagComponent::nominalOutputDb(const TruePeakLimiterParameters& model, double inputDb) noexcept
{
    return model.enabled ? std::min(inputDb - model.thresholdDb, model.ceilingDbtp) : inputDb;
}

void TruePeakLimiterMagComponent::paintModernTransferCurve(juce::Graphics& g)
{
    if (feedbackBounds.isEmpty()) return;
    const auto area = feedbackBounds.toFloat().reduced(0.5f);
    g.setColour(RendererVisualStyle::panelBackground());
    g.fillRoundedRectangle(area, 8.0f);
    g.setColour(RendererVisualStyle::borderColour().withAlpha(0.7f));
    g.drawRoundedRectangle(area, 8.0f, 1.0f);
    auto content = feedbackBounds.reduced(9, 6);
    auto caption = content.removeFromTop(15);
    g.setFont(ModernTypography::font(10.0f));
    g.setColour(RendererVisualStyle::secondaryText());
    g.drawText("Static target", caption, juce::Justification::centredLeft, true);
    if (content.getHeight() < 34) return;
    auto footer = content.removeFromBottom(14);
    const bool metrics = feedbackVisible && feedback.processedSamples > 0 && content.getHeight() >= 82;
    juce::Rectangle<int> readout;
    if (metrics) readout = content.removeFromBottom(32);
    auto plot = content.toFloat().reduced(1.0f, 5.0f);
    const auto x = [&](double db) { return plot.getX() + static_cast<float>((db + 36.0) / 36.0) * plot.getWidth(); };
    const auto y = [&](double db) { return plot.getBottom() - static_cast<float>((db + 36.0) / 36.0) * plot.getHeight(); };
    {
        juce::Graphics::ScopedSaveState scope(g);
        g.reduceClipRegion(plot.toNearestInt());
        g.setColour(RendererVisualStyle::borderColour().withAlpha(0.4f));
        for (const double db : { -24.0, -12.0 })
        {
            g.drawVerticalLine(juce::roundToInt(x(db)), plot.getY(), plot.getBottom());
            g.drawHorizontalLine(juce::roundToInt(y(db)), plot.getX(), plot.getRight());
        }
        g.setColour(RendererVisualStyle::secondaryText().withAlpha(0.22f));
        g.drawLine(plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);
        juce::Path line;
        line.startNewSubPath(x(-36.0), y(nominalOutputDb(parameters, -36.0)));
        if (parameters.enabled)
        {
            const auto knee = juce::jlimit(-36.0, 0.0, parameters.thresholdDb + parameters.ceilingDbtp);
            line.lineTo(x(knee), y(nominalOutputDb(parameters, knee)));
        }
        line.lineTo(x(0.0), y(nominalOutputDb(parameters, 0.0)));
        auto fill = line;
        fill.lineTo(plot.getRight(), plot.getBottom());
        fill.lineTo(plot.getX(), plot.getBottom());
        fill.closeSubPath();
        const auto blue = RendererVisualStyle::accentBlue();
        g.setGradientFill(juce::ColourGradient(blue.withAlpha(parameters.enabled ? 0.20f : 0.07f),
            plot.getX(), plot.getY(), blue.withAlpha(0.03f), plot.getX(), plot.getBottom(), false));
        g.fillPath(fill);
        g.setColour(parameters.enabled ? blue : RendererVisualStyle::secondaryText().withAlpha(0.6f));
        g.strokePath(line, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour(RendererVisualStyle::secondaryText());
    g.drawText("-36", footer.removeFromLeft(30), juce::Justification::centredLeft);
    g.drawText("0 dB in", footer, juce::Justification::centredRight, true);
    if (metrics)
    {
        g.setColour(RendererVisualStyle::accentBlue());
        g.drawText("TP " + formatDb(feedback.inputTruePeakDbtp, " dBTP"),
            readout.removeFromTop(16), juce::Justification::centredLeft, true);
        g.setColour(RendererVisualStyle::accentOrange());
        g.drawText("GR " + formatDb(feedback.maximumGainReductionDb, " dB"),
            readout, juce::Justification::centredLeft, true);
    }
}

void TruePeakLimiterMagComponent::updateControlsFromModel()
{
    synchronisingControls = true;
    enabledButton.setToggleState(parameters.enabled, juce::dontSendNotification);
    enabledButton.setButtonText(parameters.enabled ? "On" : "Off");
    thresholdSlider.setValue(parameters.thresholdDb, juce::dontSendNotification);
    ceilingSlider.setValue(parameters.ceilingDbtp, juce::dontSendNotification);
    releaseModeBox.setSelectedId(parameters.releaseMode == TruePeakLimiterReleaseMode::manual ? 2 : 1,
                                 juce::dontSendNotification);
    releaseSlider.setValue(parameters.releaseMs, juce::dontSendNotification);
    synchronisingControls = false;
}

void TruePeakLimiterMagComponent::updateVisibilityAndLayout()
{
    if (!RendererVisualStyle::isClassic())
    {
        updateModernLayout();
        return;
    }
    const auto summary = informationTier == RendererInformationTier::summary;
    const auto detail = informationTier == RendererInformationTier::detail;
    const auto controlsEnabled = parameters.enabled && !applying;

    auto content = getLocalBounds().reduced(summary ? 10 : 14);
    smallButton.setVisible(true);
    mediumButton.setVisible(true);
    largeButton.setVisible(true);
    smallButton.setToggleState(magSize == RendererMagSize::small, juce::dontSendNotification);
    mediumButton.setToggleState(magSize == RendererMagSize::medium, juce::dontSendNotification);
    largeButton.setToggleState(magSize == RendererMagSize::large, juce::dontSendNotification);

    if (summary)
    {
        auto sizeRow = getLocalBounds().reduced(8).removeFromTop(28);
        largeButton.setBounds(sizeRow.removeFromRight(36));
        sizeRow.removeFromRight(3);
        mediumButton.setBounds(sizeRow.removeFromRight(36));
        sizeRow.removeFromRight(3);
        smallButton.setBounds(sizeRow.removeFromRight(36));

        enabledButton.setVisible(false);
        recallButton.setVisible(false);
        previewButton.setVisible(false);
        applyButton.setVisible(false);
        for (auto* component : { static_cast<juce::Component*>(&thresholdLabel),
                                 static_cast<juce::Component*>(&thresholdSlider),
                                 static_cast<juce::Component*>(&ceilingLabel),
                                 static_cast<juce::Component*>(&ceilingSlider),
                                 static_cast<juce::Component*>(&releaseLabel),
                                 static_cast<juce::Component*>(&releaseModeBox),
                                 static_cast<juce::Component*>(&releaseSlider),
                                 static_cast<juce::Component*>(&lockedFactsLabel) })
            component->setVisible(false);
        feedbackBounds = {};
        return;
    }

    content.removeFromTop(46);
    auto header = content.removeFromTop(28);
    largeButton.setBounds(header.removeFromRight(36));
    header.removeFromRight(4);
    mediumButton.setBounds(header.removeFromRight(36));
    header.removeFromRight(4);
    smallButton.setBounds(header.removeFromRight(36));
    header.removeFromRight(8);
    enabledButton.setVisible(true);
    enabledButton.setBounds(header.removeFromLeft(68));

    auto actionRow = content.removeFromBottom(30);
    applyButton.setVisible(true);
    previewButton.setVisible(true);
    applyButton.setBounds(actionRow.removeFromRight(72));
    actionRow.removeFromRight(6);
    previewButton.setBounds(actionRow.removeFromRight(summary ? 92 : 112));
    actionRow.removeFromRight(6);
    recallButton.setBounds(actionRow.removeFromRight(summary ? 0 : 106));
    recallButton.setVisible(!summary);
    recallButton.setEnabled(recallAvailable && !applying);
    previewButton.setEnabled(controlsEnabled);
    applyButton.setEnabled(controlsEnabled);
    enabledButton.setEnabled(!applying);

    for (auto* component : { static_cast<juce::Component*>(&thresholdLabel),
                             static_cast<juce::Component*>(&thresholdSlider),
                             static_cast<juce::Component*>(&ceilingLabel),
                             static_cast<juce::Component*>(&ceilingSlider),
                             static_cast<juce::Component*>(&releaseLabel),
                             static_cast<juce::Component*>(&releaseModeBox),
                             static_cast<juce::Component*>(&releaseSlider),
                             static_cast<juce::Component*>(&lockedFactsLabel) })
    {
        component->setVisible(!summary);
        component->setEnabled(controlsEnabled);
    }
    releaseLabel.setVisible(detail);
    releaseModeBox.setVisible(detail);
    releaseSlider.setVisible(detail);
    lockedFactsLabel.setVisible(detail);
    feedbackBounds = {};

    content.removeFromTop(8);
    const auto layoutParameterRow = [&content](juce::Label& label, juce::Component& control)
    {
        auto row = content.removeFromTop(std::min(34, content.getHeight()));
        label.setBounds(row.removeFromLeft(std::min(92, row.getWidth())));
        control.setBounds(row);
        content.removeFromTop(std::min(4, content.getHeight()));
    };
    layoutParameterRow(thresholdLabel, thresholdSlider);
    layoutParameterRow(ceilingLabel, ceilingSlider);
    if (detail)
    {
        auto releaseRow = content.removeFromTop(std::min(34, content.getHeight()));
        releaseLabel.setBounds(releaseRow.removeFromLeft(std::min(92, releaseRow.getWidth())));
        releaseModeBox.setBounds(releaseRow.removeFromLeft(std::min(88, releaseRow.getWidth())));
        releaseRow.removeFromLeft(std::min(6, releaseRow.getWidth()));
        releaseSlider.setBounds(releaseRow);
        content.removeFromTop(std::min(4, content.getHeight()));
        lockedFactsLabel.setBounds(content.removeFromTop(std::min(24, content.getHeight())));
        content.removeFromTop(std::min(6, content.getHeight()));
    }
    if (content.getHeight() >= 44)
        feedbackBounds = content;
}

void TruePeakLimiterMagComponent::updateModernLayout()
{
    getProperties().set("workspace.headerHeight", ModernPanelPainter::headerHeight);
    feedbackBounds = {};
    modernStatusBounds = {};
    auto content = getLocalBounds().withTrimmedTop(ModernPanelPainter::headerHeight).reduced(8, 6);
    const bool summary = informationTier == RendererInformationTier::summary || getHeight() < 172;
    const bool controlsEnabled = parameters.enabled && !applying;
    auto header = content.removeFromTop(std::min(24, content.getHeight()));
    for (auto* button : { &largeButton, &mediumButton, &smallButton })
    {
        button->setVisible(true);
        button->setBounds(header.removeFromRight(std::min(22, header.getWidth())));
        header.removeFromRight(std::min(2, header.getWidth()));
    }
    smallButton.setToggleState(magSize == RendererMagSize::small, juce::dontSendNotification);
    mediumButton.setToggleState(magSize == RendererMagSize::medium, juce::dontSendNotification);
    largeButton.setToggleState(magSize == RendererMagSize::large, juce::dontSendNotification);
    enabledButton.setVisible(true);
    enabledButton.setEnabled(!applying);
    enabledButton.setBounds(header.removeFromLeft(std::min(60, header.getWidth())));
    content.removeFromTop(std::min(4, content.getHeight()));
    for (auto* component : std::initializer_list<juce::Component*> {
             &thresholdLabel, &thresholdSlider, &ceilingLabel, &ceilingSlider,
             &releaseLabel, &releaseModeBox, &releaseSlider, &lockedFactsLabel,
             &recallButton, &previewButton, &applyButton })
        component->setVisible(false);
    if (summary)
    {
        modernStatusBounds = content;
        return;
    }

    const bool narrow = content.getWidth() < 280;
    auto actions = content.removeFromTop(24);
    const int actionWidth = std::min(100, (actions.getWidth() - 8) / 3);
    recallButton.setButtonText(narrow ? "Last" : "Last Applied");
    previewButton.setButtonText(previewing ? (narrow ? "Stop" : "Stop Preview") : "Preview");
    for (auto* button : { &applyButton, &previewButton, &recallButton })
    {
        button->setVisible(true);
        button->setBounds(actions.removeFromRight(actionWidth));
        actions.removeFromRight(std::min(4, actions.getWidth()));
    }
    recallButton.setEnabled(recallAvailable && !applying);
    previewButton.setEnabled(controlsEnabled);
    applyButton.setEnabled(controlsEnabled);
    content.removeFromTop(std::min(6, content.getHeight()));

    if (content.getHeight() >= 76)
        modernStatusBounds = content.removeFromBottom(16);
    const bool sideBySide = content.getWidth() >= 336 && content.getHeight() >= 120;
    if (sideBySide)
    {
        const int controlsWidth = juce::jlimit(210, 280, content.getWidth() * 3 / 5);
        feedbackBounds = content.withTrimmedLeft(controlsWidth + 10);
        content.setWidth(controlsWidth);
    }
    const bool showRelease = content.getHeight() >= 114;
    // The normal side-by-side layout reserves at least 210 px for parameters.
    // Keep full labels there; the narrow stack retains its shorter label/value
    // widths so the actual slider still has useful travel.
    const bool compactParameters = content.getWidth() < 200;
    const int rowHeight = sideBySide ? juce::jlimit(28, 48, content.getHeight() / 5) : 26;
    const auto parameterRow = [&](juce::Label& label, juce::Slider& slider, const char* title)
    {
        auto row = content.removeFromTop(rowHeight);
        label.setText(compactParameters && &label == &thresholdLabel ? "Thresh." : title, juce::dontSendNotification);
        label.setVisible(true);
        // These are descriptions, not controls. They must remain readable when
        // the processor is off or an Apply operation temporarily locks editing.
        label.setEnabled(true);
        label.setFont(ModernTypography::font(11.0f));
        label.setMinimumHorizontalScale(1.0f);
        label.setBounds(row.removeFromLeft(compactParameters ? 48 : 60));
        slider.setVisible(true);
        slider.setEnabled(controlsEnabled);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, compactParameters ? 58 : 70, 22);
        slider.setBounds(row);
        content.removeFromTop(std::min(2, content.getHeight()));
    };
    parameterRow(thresholdLabel, thresholdSlider, "Threshold");
    parameterRow(ceilingLabel, ceilingSlider, "Ceiling");
    // Release is useful in either normal tier whenever complete rows fit.
    // A short tile still exposes both essential values and all size controls.
    if (showRelease && content.getHeight() >= 54)
    {
        auto row = content.removeFromTop(26);
        releaseLabel.setText("Release", juce::dontSendNotification);
        releaseLabel.setVisible(true);
        releaseLabel.setEnabled(true);
        releaseLabel.setFont(ModernTypography::font(11.0f));
        releaseLabel.setBounds(row.removeFromLeft(58));
        releaseModeBox.setVisible(true);
        releaseModeBox.setEnabled(controlsEnabled);
        releaseModeBox.setBounds(row.removeFromLeft(std::min(88, row.getWidth())));
        content.removeFromTop(2);
        releaseSlider.setVisible(true);
        releaseSlider.setEnabled(controlsEnabled && parameters.releaseMode == TruePeakLimiterReleaseMode::manual);
        releaseSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
        releaseSlider.setBounds(content.removeFromTop(26));
        content.removeFromTop(std::min(4, content.getHeight()));
    }
    if (!sideBySide && content.getHeight() >= 68)
        feedbackBounds = content;
    setTooltip("Static target shows input gain and the public ceiling. Lookahead, release and the true-peak safety guard affect actual output; TP/GR readings require Preview or Apply.");
}

bool TruePeakLimiterMagComponent::areSizeControlsUsableForTesting() const noexcept
{
    const auto bounds = getLocalBounds();
    return smallButton.isVisible() && mediumButton.isVisible() && largeButton.isVisible()
        && !smallButton.getBounds().isEmpty()
        && bounds.contains(smallButton.getBounds())
        && bounds.contains(mediumButton.getBounds())
        && bounds.contains(largeButton.getBounds());
}

void TruePeakLimiterMagComponent::emitParametersChanged()
{
    updateControlsFromModel();
    if (!parameters.enabled)
    {
        statusText = "Off | zero processing";
        if (previewing && onPreviewRequested)
            onPreviewRequested(false);
    }
    else
    {
        statusText = "Ready | Preview is audition-only";
    }
    if (onParametersChanged)
        onParametersChanged(parameters);
    updateVisibilityAndLayout();
    repaint();
}

void TruePeakLimiterMagComponent::requestMagSize(RendererMagSize size)
{
    magSize = size;
    if (onMagSizeRequested)
        onMagSizeRequested(size);
    updateVisibilityAndLayout();
    repaint();
}

juce::String TruePeakLimiterMagComponent::formatDb(double value,
                                                   const juce::String& suffix)
{
    return std::isfinite(value) ? juce::String(value, 1) + suffix : "--" + suffix;
}

void TruePeakLimiterMagComponent::setEnabledForTesting(bool enabled)
{
    enabledButton.setToggleState(enabled, juce::sendNotificationSync);
}

bool TruePeakLimiterMagComponent::isFeedbackVisibleForTesting() const noexcept
{
    return feedbackVisible;
}

const juce::String& TruePeakLimiterMagComponent::getStatusTextForTesting() const noexcept
{
    return statusText;
}
} // namespace singleeditor
