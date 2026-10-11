#include "UI/Components/MetadataSidebarComponent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/AmbisonicBinauralFilterBank.h"
#include "Audio/ChannelRouter.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernPanelPainter.h"

namespace singleeditor
{
namespace
{
struct InspectorItem
{
    juce::String label;
    juce::String value;
};

void drawInspectorItems(juce::Graphics& g,
                        juce::Rectangle<int>& area,
                        std::initializer_list<InspectorItem> items,
                        int valueHeight = 20)
{
    for (const auto& item : items)
    {
        if (item.value.isEmpty())
            continue;

        auto labelArea = area.removeFromTop(13);
        g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.9f));
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText(item.label.toUpperCase(), labelArea, juce::Justification::centredLeft);

        auto valueArea = area.removeFromTop(valueHeight);
        g.setColour(ModernAppleLookAndFeel::primaryText());
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        // Use drawText with ellipsis for single-line values, drawFittedText for multi-line hints
        if (item.value.contains("\n") || item.value.length() > 40)
            g.drawText(item.value, valueArea, juce::Justification::centredLeft, true);
        else
            g.drawText(item.value, valueArea, juce::Justification::centredLeft, true);
        area.removeFromTop(8);
    }
}

void drawSectionTitle(juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& title)
{
    g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.92f));
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawText(title.toUpperCase(), area.removeFromTop(14), juce::Justification::centredLeft);
    area.removeFromTop(4);
}

class OutputMappingMatrixComponent final : public juce::Component
{
public:
    OutputMappingMatrixComponent(AudioEngine& engineToUse,
                                 ChannelLayoutState targetLayoutToUse)
        : engine(engineToUse),
          targetLayout(std::move(targetLayoutToUse)),
          outputCount(targetLayout.sourceChannelCount)
    {
        const auto existing = engine.getOutputMapping();
        const auto usableExisting = ChannelRouter::outputMappingIsValid(targetLayout, existing)
                                  ? existing
                                  : ChannelOutputMapping {};
        for (const auto role : targetLayout.selectedRoles)
        {
            selectedOutputs.push_back(
                ChannelRouter::outputChannelForRole(targetLayout, usableExisting, role));
        }

        for (int roleIndex = 0; roleIndex < static_cast<int>(targetLayout.selectedRoles.size()); ++roleIndex)
        {
            for (int output = 0; output < outputCount; ++output)
            {
                auto button = std::make_unique<juce::TextButton>();
                button->setTooltip(ChannelLayoutCatalog::roleToAbbreviation(
                                       targetLayout.selectedRoles[static_cast<std::size_t>(roleIndex)])
                                   + " -> Output " + juce::String(output + 1));
                button->onClick = [this, roleIndex, output]
                {
                    selectOutput(roleIndex, output);
                };
                addAndMakeVisible(*button);
                matrixButtons.push_back(std::move(button));
            }
        }

        applyButton.setButtonText("Apply");
        applyButton.onClick = [this]
        {
            applyMapping();
        };
        addAndMakeVisible(applyButton);

        resetButton.setButtonText("Default");
        resetButton.onClick = [this]
        {
            resetToDefault();
        };
        addAndMakeVisible(resetButton);

        cancelButton.setButtonText("Cancel");
        cancelButton.onClick = [this]
        {
            closeDialog();
        };
        addAndMakeVisible(cancelButton);

        setSize(juce::jmax(560, 150 + (outputCount * 38)),
                juce::jmax(360, 150 + (static_cast<int>(targetLayout.selectedRoles.size()) * 34)));
        refreshButtons();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(ModernAppleLookAndFeel::windowBackground());
        g.setColour(ModernAppleLookAndFeel::primaryText());
        g.setFont(juce::Font(juce::FontOptions(19.0f, juce::Font::bold)));
        g.drawText("Speaker Role -> Device Output",
                   getLocalBounds().reduced(24).removeFromTop(28),
                   juce::Justification::centredLeft);

        g.setColour(ModernAppleLookAndFeel::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText("Physical routing for this app session only. Source channel order is unchanged.",
                   juce::Rectangle<int>(24, 54, getWidth() - 48, 20),
                   juce::Justification::centredLeft);

        constexpr int labelWidth = 112;
        constexpr int cellWidth = 38;
        const auto gridX = 24 + labelWidth;
        const auto gridY = 100;
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        for (int output = 0; output < outputCount; ++output)
        {
            g.drawText(juce::String(output + 1),
                       gridX + (output * cellWidth),
                       gridY - 24,
                       cellWidth - 4,
                       20,
                       juce::Justification::centred);
        }
        for (int roleIndex = 0; roleIndex < static_cast<int>(targetLayout.selectedRoles.size()); ++roleIndex)
        {
            g.drawText(ChannelLayoutCatalog::roleToAbbreviation(
                           targetLayout.selectedRoles[static_cast<std::size_t>(roleIndex)]),
                       24,
                       gridY + (roleIndex * 34),
                       labelWidth - 8,
                       28,
                       juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        constexpr int labelWidth = 112;
        constexpr int cellWidth = 38;
        constexpr int gridY = 100;
        const auto gridX = 24 + labelWidth;
        for (int roleIndex = 0; roleIndex < static_cast<int>(targetLayout.selectedRoles.size()); ++roleIndex)
        {
            for (int output = 0; output < outputCount; ++output)
            {
                const auto index = (roleIndex * outputCount) + output;
                matrixButtons[static_cast<std::size_t>(index)]->setBounds(
                    gridX + (output * cellWidth),
                    gridY + (roleIndex * 34),
                    cellWidth - 4,
                    28);
            }
        }

        auto buttons = getLocalBounds().reduced(24).removeFromBottom(34);
        applyButton.setBounds(buttons.removeFromRight(86));
        buttons.removeFromRight(8);
        cancelButton.setBounds(buttons.removeFromRight(86));
        resetButton.setBounds(buttons.removeFromLeft(86));
    }

private:
    void selectOutput(int roleIndex, int output)
    {
        if (!juce::isPositiveAndBelow(roleIndex, static_cast<int>(selectedOutputs.size()))
            || !juce::isPositiveAndBelow(output, outputCount))
        {
            return;
        }

        const auto previousOutput = selectedOutputs[static_cast<std::size_t>(roleIndex)];
        for (int otherRole = 0; otherRole < static_cast<int>(selectedOutputs.size()); ++otherRole)
        {
            if (otherRole != roleIndex
                && selectedOutputs[static_cast<std::size_t>(otherRole)] == output)
            {
                selectedOutputs[static_cast<std::size_t>(otherRole)] = previousOutput;
                break;
            }
        }
        selectedOutputs[static_cast<std::size_t>(roleIndex)] = output;
        refreshButtons();
    }

    void resetToDefault()
    {
        ChannelOutputMapping defaults;
        for (int roleIndex = 0; roleIndex < static_cast<int>(targetLayout.selectedRoles.size()); ++roleIndex)
        {
            selectedOutputs[static_cast<std::size_t>(roleIndex)] =
                ChannelRouter::outputChannelForRole(
                    targetLayout,
                    defaults,
                    targetLayout.selectedRoles[static_cast<std::size_t>(roleIndex)]);
        }
        refreshButtons();
    }

    void refreshButtons()
    {
        for (int roleIndex = 0; roleIndex < static_cast<int>(selectedOutputs.size()); ++roleIndex)
        {
            for (int output = 0; output < outputCount; ++output)
            {
                const auto index = (roleIndex * outputCount) + output;
                const auto selected = selectedOutputs[static_cast<std::size_t>(roleIndex)] == output;
                auto& button = *matrixButtons[static_cast<std::size_t>(index)];
                button.setButtonText(selected ? "X" : "");
                button.setColour(juce::TextButton::buttonColourId,
                                 selected ? ModernAppleLookAndFeel::accentBlue()
                                          : ModernAppleLookAndFeel::mutedSurface());
                button.setColour(juce::TextButton::textColourOffId,
                                 selected ? juce::Colours::white
                                          : ModernAppleLookAndFeel::secondaryText());
            }
        }
    }

    void applyMapping()
    {
        ChannelOutputMapping mapping;
        auto differsFromDefault = false;
        for (int roleIndex = 0; roleIndex < static_cast<int>(targetLayout.selectedRoles.size()); ++roleIndex)
        {
            const auto role = targetLayout.selectedRoles[static_cast<std::size_t>(roleIndex)];
            const auto output = selectedOutputs[static_cast<std::size_t>(roleIndex)];
            mapping.setOutputChannel(role, output);
            differsFromDefault = differsFromDefault
                              || output != ChannelRouter::outputChannelForRole(
                                               targetLayout,
                                               ChannelOutputMapping {},
                                               role);
        }

        if (differsFromDefault && ChannelRouter::outputMappingIsValid(targetLayout, mapping))
            engine.setOutputMapping(mapping);
        else
            engine.clearOutputMapping();
        closeDialog();
    }

    void closeDialog()
    {
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
            dialog->exitModalState(0);
    }

    AudioEngine& engine;
    ChannelLayoutState targetLayout;
    int outputCount = 0;
    std::vector<int> selectedOutputs;
    std::vector<std::unique_ptr<juce::TextButton>> matrixButtons;
    juce::TextButton applyButton;
    juce::TextButton resetButton;
    juce::TextButton cancelButton;
};

class AmbisonicOrientationComponent final : public juce::Component
{
public:
    AmbisonicOrientationComponent(AudioEngine& engineToUse,
                                  SingleTrackDocument& documentToUse,
                                  std::function<void()> orientationChangedToUse)
        : engine(engineToUse),
          document(documentToUse),
          sourceFile(documentToUse.getFileInfo().file),
          orientationChanged(std::move(orientationChangedToUse))
    {
        configureSlider(yawSlider, -180.0, 180.0, "Yaw");
        configureSlider(pitchSlider, -90.0, 90.0, "Pitch");
        configureSlider(rollSlider, -180.0, 180.0, "Roll");

        const auto orientation = document.getAmbisonicOrientation();
        yawSlider.setValue(orientation.yawDegrees, juce::dontSendNotification);
        pitchSlider.setValue(orientation.pitchDegrees, juce::dontSendNotification);
        rollSlider.setValue(orientation.rollDegrees, juce::dontSendNotification);

        yawSlider.onValueChange = [this] { orientationValueChanged(); };
        pitchSlider.onValueChange = [this] { orientationValueChanged(); };
        rollSlider.onValueChange = [this] { orientationValueChanged(); };

        resetButton.setButtonText("Reset");
        resetButton.onClick = [this]
        {
            suppressChanges = true;
            yawSlider.setValue(0.0, juce::dontSendNotification);
            pitchSlider.setValue(0.0, juce::dontSendNotification);
            rollSlider.setValue(0.0, juce::dontSendNotification);
            suppressChanges = false;
            commitOrientation();
        };
        addAndMakeVisible(resetButton);

        closeButton.setButtonText("Close");
        closeButton.onClick = [this]
        {
            if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                dialog->exitModalState(0);
        };
        addAndMakeVisible(closeButton);
        setSize(480, 286);
    }

    ~AmbisonicOrientationComponent() override
    {
        engine.clearAmbisonicOrientationPreview();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(ModernAppleLookAndFeel::windowBackground());
        g.setColour(ModernAppleLookAndFeel::primaryText());
        g.setFont(juce::Font(juce::FontOptions(19.0f, juce::Font::bold)));
        g.drawText("Ambisonic Orientation",
                   getLocalBounds().reduced(24).removeFromTop(28),
                   juce::Justification::centredLeft);
        g.setColour(ModernAppleLookAndFeel::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText("Active sound-field rotation. Positive yaw turns the scene left; positive pitch turns it up.",
                   juce::Rectangle<int>(24, 54, getWidth() - 48, 20),
                   juce::Justification::centredLeft);

        g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        g.drawText("Yaw", 24, 96, 58, 30, juce::Justification::centredLeft);
        g.drawText("Pitch", 24, 140, 58, 30, juce::Justification::centredLeft);
        g.drawText("Roll", 24, 184, 58, 30, juce::Justification::centredLeft);
    }

    void resized() override
    {
        yawSlider.setBounds(82, 96, getWidth() - 106, 30);
        pitchSlider.setBounds(82, 140, getWidth() - 106, 30);
        rollSlider.setBounds(82, 184, getWidth() - 106, 30);
        auto buttons = getLocalBounds().reduced(24).removeFromBottom(34);
        closeButton.setBounds(buttons.removeFromRight(86));
        resetButton.setBounds(buttons.removeFromLeft(86));
    }

private:
    void configureSlider(juce::Slider& slider,
                         double minimum,
                         double maximum,
                         const juce::String& name)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 74, 24);
        slider.setRange(minimum, maximum, 1.0);
        slider.setNumDecimalPlacesToDisplay(0);
        slider.setTextValueSuffix(" deg");
        slider.setChangeNotificationOnlyOnRelease(false);
        slider.setTooltip(name + " rotation in degrees. Speaker and Binaural monitoring update while dragging.");
        slider.onDragStart = [this]
        {
            sliderDragActive = true;
        };
        slider.onDragEnd = [this]
        {
            sliderDragActive = false;
            commitOrientation();
        };
        addAndMakeVisible(slider);
    }

    AmbisonicOrientation currentOrientation() const noexcept
    {
        return { static_cast<float>(yawSlider.getValue()),
                 static_cast<float>(pitchSlider.getValue()),
                 static_cast<float>(rollSlider.getValue()) };
    }

    void orientationValueChanged()
    {
        if (suppressChanges)
            return;
        if (document.getFileInfo().file != sourceFile)
            return;

        if (sliderDragActive)
        {
            engine.setAmbisonicOrientationPreview(currentOrientation());
            return;
        }

        commitOrientation();
    }

    void commitOrientation()
    {
        if (suppressChanges || document.getFileInfo().file != sourceFile)
            return;

        if (document.setAmbisonicOrientation(currentOrientation())
            && orientationChanged)
        {
            orientationChanged();
        }
        engine.clearAmbisonicOrientationPreview();
    }

    AudioEngine& engine;
    SingleTrackDocument& document;
    juce::File sourceFile;
    std::function<void()> orientationChanged;
    juce::Slider yawSlider;
    juce::Slider pitchSlider;
    juce::Slider rollSlider;
    juce::TextButton resetButton;
    juce::TextButton closeButton;
    bool suppressChanges = false;
    bool sliderDragActive = false;
};

juce::String ambisonicMonitorDescription(const ChannelLayoutState& layout,
                                         AmbisonicMonitorMode mode,
                                         const juce::String& status)
{
    if (layout.isAmbisonic && mode == AmbisonicMonitorMode::binauralHrtf)
        return "Binaural HRTF / SADIE 006";
    if (layout.isAmbisonic && status.isNotEmpty())
        return "Speaker projection; HRTF unavailable: " + status;

    switch (AmbisonicMonitorDecoder::statusFor(layout))
    {
        case AmbisonicMonitorDecodeStatus::ready:
            return "ACN/SN3D decode and rotation ready on demand";
        case AmbisonicMonitorDecodeStatus::unsupportedOrder:
            return "Higher-order monitor decode pending";
        case AmbisonicMonitorDecodeStatus::unsupportedNormalization:
            return "Normalization is not supported";
        case AmbisonicMonitorDecodeStatus::notRequired:
            break;
    }

    return {};
}
}

class MetadataSidebarComponent::BankInspectorContent final : public juce::Component
{
public:
    void setItems(std::vector<InspectorItem> items)
    {
        if (items.size() == fields.size()
            && std::equal(items.begin(), items.end(), fields.begin(),
                [] (const auto& item, const auto& field)
                { return item.label == field->item.label && item.value == field->item.value; }))
            return;

        fields.clear();
        for (auto& item : items)
        {
            auto field = std::make_unique<Field>(std::move(item));
            addAndMakeVisible(*field);
            fields.push_back(std::move(field));
        }
    }

    void layoutForWidth(int width)
    {
        int y = 4;
        for (auto& field : fields)
        {
            const auto height = field->heightForWidth(width);
            field->setBounds(0, y, width, height);
            y += height + 10;
        }
        setSize(width, y + 4);
    }

private:
    struct Field final : juce::Component
    {
        explicit Field(InspectorItem value) : item(std::move(value))
        {
            setName(item.label.isEmpty() ? "Bank note" : item.label);
            heading.setText(item.label.toUpperCase(), juce::dontSendNotification);
            heading.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            body.setText(item.value, juce::dontSendNotification);
            body.setFont(juce::Font(juce::FontOptions(13.0f)));
            body.setTitle(getName());
            body.setTooltip(item.value);
            for (auto* label : { &heading, &body })
            {
                label->setBorderSize({});
                label->setJustificationType(juce::Justification::topLeft);
                label->setMinimumHorizontalScale(1.0f);
                addAndMakeVisible(*label);
            }
            lookAndFeelChanged();
        }

        int heightForWidth(int width) const
        {
            juce::AttributedString text;
            text.append(item.value, body.getFont());
            juce::TextLayout layout;
            layout.createLayout(text, static_cast<float>(juce::jmax(1, width)));
            return (item.label.isEmpty() ? 0 : 17)
                + (item.value.isEmpty() ? 0 : juce::jmax(20, juce::roundToInt(std::ceil(layout.getHeight())) + 4));
        }

        void resized() override
        {
            auto area = getLocalBounds();
            heading.setBounds(item.label.isEmpty() ? juce::Rectangle<int>() : area.removeFromTop(17));
            body.setBounds(area);
        }

        void lookAndFeelChanged() override
        {
            heading.setColour(juce::Label::textColourId, ModernAppleLookAndFeel::secondaryText());
            body.setColour(juce::Label::textColourId, ModernAppleLookAndFeel::primaryText());
        }

        InspectorItem item;
        juce::Label heading, body;
    };
    std::vector<std::unique_ptr<Field>> fields;
};

class MetadataSidebarComponent::BinauralPreparation final : private juce::Thread, private juce::AsyncUpdater
{
public:
    using Completion = std::function<void(std::shared_ptr<const AmbisonicBinauralFilterBank>, juce::String)>;
    explicit BinauralPreparation(Completion callback)
        : juce::Thread("Ambisonic HRTF preparation"), completion(std::move(callback)) {}
    ~BinauralPreparation() override
    {
        signalThreadShouldExit();
        stopThread(-1);
        cancelPendingUpdate();
    }
    void begin() { startThread(juce::Thread::Priority::low); }
private:
    void run() override
    {
        bank = AmbisonicBinauralFilterBank::loadDefault(error);
        if (!threadShouldExit()) triggerAsyncUpdate();
    }
    void handleAsyncUpdate() override
    {
        // Join before reading worker results. The UI callback may release us.
        stopThread(-1);
        const auto callback = completion;
        auto result = std::move(bank);
        auto message = error;
        if (callback) callback(std::move(result), std::move(message));
    }
    Completion completion;
    std::shared_ptr<const AmbisonicBinauralFilterBank> bank;
    juce::String error;
};

MetadataSidebarComponent::~MetadataSidebarComponent() = default;

void MetadataSidebarComponent::requestBinauralMonitor()
{
    binauralPreparation.reset();
    const auto expected = document.getState();
    pendingBinauralState = expected;
    const juce::Component::SafePointer<MetadataSidebarComponent> safe(this);
    binauralPreparation = std::make_unique<BinauralPreparation>(
        [safe, expected](std::shared_ptr<const AmbisonicBinauralFilterBank> bank, juce::String error)
        {
            if (!safe || safe->pendingBinauralState != expected) return;
            safe->binauralPreparation.reset();
            safe->pendingBinauralState.reset();
            if (safe->document.getState() == expected)
            {
                const auto applied = safe->document.applyPreparedBinauralMonitor(expected, std::move(bank), error);
                if (!applied && error.isNotEmpty())
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                        "Binaural HRTF unavailable", error);
            }
            safe->refreshChannelControls();
        });
    binauralPreparation->begin();
    refreshChannelControls();
}

MetadataSidebarComponent::MetadataSidebarComponent(AudioEngine& engineToUse,
                                                   SingleTrackDocument& documentToUse,
                                                   BnkDocument& bnkDocumentToUse)
    : engine(engineToUse),
      document(documentToUse),
      bnkDocument(bnkDocumentToUse)
{
    channelOrderBox.setName("Source Channel Interpretation");
    channelOrderBox.setTitle("Source Channel Interpretation");
    channelOrderBox.setComponentID("source-channel-interpretation");
    channelOrderBox.setTooltip("Interpret this file's source channel order. This does not reorder device outputs.");
    channelOrderBox.onChange = [this]
    {
        if (suppressChannelCallbacks)
            return;

        const auto selectedIndex = channelOrderBox.getSelectedItemIndex();
        if (juce::isPositiveAndBelow(selectedIndex, channelOrderPresetIds.size()))
        {
            if (document.setChannelLayoutPreset(channelOrderPresetIds[selectedIndex]))
                refreshChannelControls();
        }
    };
    addAndMakeVisible(channelOrderBox);

    monitorModeBox.setTooltip("Choose how this source is monitored on the current output device.");
    monitorModeBox.addItem("Auto device", static_cast<int>(MonitorMode::autoFollowDevice) + 1);
    monitorModeBox.addItem("Multichannel", static_cast<int>(MonitorMode::forceMultichannel) + 1);
    monitorModeBox.addItem("Stereo downmix", static_cast<int>(MonitorMode::forceStereoDownmix) + 1);
    monitorModeBox.onChange = [this]
    {
        if (suppressChannelCallbacks)
            return;

        const auto mode = static_cast<MonitorMode>(juce::jlimit(0, 2, monitorModeBox.getSelectedId() - 1));
        engine.setMonitorMode(mode);
        repaint();
    };
    addAndMakeVisible(monitorModeBox);

    pingRoleBox.setTooltip("Choose a speaker role to ping through the current monitor routing.");
    addAndMakeVisible(pingRoleBox);

    pingRoleButton.setButtonText("Ping");
    pingRoleButton.setTooltip("Play a short low-level speaker test signal for the selected role.");
    pingRoleButton.onClick = [this]
    {
        const auto roleId = pingRoleBox.getSelectedId();
        if (!document.hasAudio() || roleId <= 0)
            return;

        // Bind the displayed role itself, rather than an index into a separately
        // refreshed engine snapshot after a source-interpretation change.
        const auto role = static_cast<SpeakerRole>(roleId - 1);
        if (role != SpeakerRole::unknown && role != SpeakerRole::discrete
            && !ChannelLayoutCatalog::isAmbisonicRole(role))
            engine.pingSpeakerRole(role);
    };
    addAndMakeVisible(pingRoleButton);

    includeLfeToggle.setButtonText("Include LFE in stereo downmix");
    includeLfeToggle.setTooltip("Default off. Only affects stereo/headphone preview, not multichannel direct output.");
    includeLfeToggle.onClick = [this]
    {
        engine.setIncludeLfeInDownmix(includeLfeToggle.getToggleState());
        repaint();
    };
    addAndMakeVisible(includeLfeToggle);

    outputMappingButton.setButtonText("Output mapping...");
    outputMappingButton.setTooltip("Map semantic speaker roles to physical device output channels.");
    outputMappingButton.onClick = [this]
    {
        showOutputMappingDialog();
    };
    addAndMakeVisible(outputMappingButton);

    if (!RendererVisualStyle::isClassic())
    {
        getProperties().set("workspace.headerHeight", ModernPanelPainter::headerHeight);
        modernViewport.setViewedComponent(&modernContent, false);
        modernViewport.setScrollBarsShown(true, false);
        modernViewport.setScrollBarThickness(8);
        addAndMakeVisible(modernViewport);
        modernContent.addAndMakeVisible(fileSummary);
        for (auto* button : { &monitorDisclosure, &detailsDisclosure })
        {
            button->setClickingTogglesState(true);
            button->getProperties().set("ghostButton", true);
            button->onClick = [this, button]
            {
                resized();
                if (button->getToggleState())
                    modernViewport.setViewPosition(0, juce::jmax(0, button->getY() - 8));
                repaint();
            };
            modernContent.addAndMakeVisible(button);
        }
        monitorDisclosure.setTitle("Show channel monitoring controls");
        detailsDisclosure.setTitle("Show detailed metadata");
        for (auto* component : std::array<juce::Component*, 6> { &channelOrderBox, &monitorModeBox,
                &pingRoleBox, &pingRoleButton, &includeLfeToggle, &outputMappingButton })
            modernContent.addAndMakeVisible(component);
    }
    refreshChannelControls();
}

void MetadataSidebarComponent::beginTypedMetadataGeneration(std::uint64_t generation)
{
    typedMetadataGeneration = generation;
    clearTypedMetadataInspector();
}

void MetadataSidebarComponent::clearTypedMetadataInspector()
{
    if (artworkLoader != nullptr) artworkLoader->cancel();
    fileSummary.setInfo({});
    if (typedMetadataInspector != nullptr)
    {
        removeChildComponent(typedMetadataInspector.get());
        typedMetadataInspector.reset();
    }
    resized();
    repaint();
}

bool MetadataSidebarComponent::activateTypedMetadataInspector(
    std::uint64_t generation,
    const SingleTrackDocument::FileInfo& info)
{
    if (generation == 0
        || generation != typedMetadataGeneration
        || bnkDocument.getBankInfo().isValid())
    {
        return false;
    }

    if (typedMetadataInspector == nullptr)
    {
        auto inspector = std::make_unique<TypedMetadataInspectorComponent>();
        inspector->beginGeneration(generation);
        if (RendererVisualStyle::isClassic()) addAndMakeVisible(*inspector);
        else modernContent.addAndMakeVisible(*inspector);
        typedMetadataInspector = std::move(inspector);
    }

    if (!typedMetadataInspector->activatePostCore(generation, info))
        return false;

    fileSummary.setInfo(info);
    if (!RendererVisualStyle::isClassic())
    {
        if (artworkLoader == nullptr) artworkLoader = std::make_unique<LocalArtworkLoader>();
        artworkLoader->request(info.file, [this, generation](const juce::File& source, const juce::Image& image)
        {
            if (generation == typedMetadataGeneration) fileSummary.setArtwork(source, image);
        });
    }
    resized();
    repaint();
    return true;
}

bool MetadataSidebarComponent::refreshTypedMetadataFacts(
    std::uint64_t generation,
    const SingleTrackDocument::FileInfo& info)
{
    if (typedMetadataInspector == nullptr
        || generation != typedMetadataGeneration)
    {
        return false;
    }

    if (!typedMetadataInspector->refreshFacts(generation, info)) return false;
    fileSummary.setInfo(info);
    return true;
}

void MetadataSidebarComponent::setBnkPreviewContext(juce::String switchLabel,
                                                    juce::String switchToken,
                                                    juce::String stateLabel,
                                                    juce::String rtpcLabel)
{
    currentBnkSwitchLabel = std::move(switchLabel);
    currentBnkSwitchToken = std::move(switchToken);
    currentBnkStateLabel = std::move(stateLabel);
    currentBnkRtpcLabel = std::move(rtpcLabel);
    resized();
    repaint();
}

void MetadataSidebarComponent::showOutputMappingDialog()
{
    auto* availableManager = engine.getDeviceManagerForConfiguration();
    if (availableManager == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Output Mapping",
            "The audio device is busy with a diagnostic probe. Try again after restoration.");
        return;
    }
    auto& deviceManager = *availableManager;
    const auto setup = deviceManager.getAudioDeviceSetup();
    auto outputCount = setup.outputChannels.countNumberOfSetBits();
    if (outputCount <= 0)
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            outputCount = device->getOutputChannelNames().size();
    }

    auto targetLayout = ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(outputCount);
    if (outputCount <= 2
        || targetLayout.isDiscreteFallback
        || static_cast<int>(targetLayout.selectedRoles.size()) != outputCount)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "Output Mapping",
            "Choose a supported Quad, 5.1, 7.1, or 7.1.4 output device before editing speaker mapping.");
        return;
    }

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Output Mapping";
    options.content.setOwned(new OutputMappingMatrixComponent(engine, std::move(targetLayout)));
    options.componentToCentreAround = this;
    options.useNativeTitleBar = false;
    options.resizable = true;
    options.escapeKeyTriggersCloseButton = true;
    options.dialogBackgroundColour = ModernAppleLookAndFeel::windowBackground();
    if (auto* dialog = options.launchAsync())
    {
        dialog->setResizable(true, false);
        dialog->centreWithSize(dialog->getWidth(), dialog->getHeight());
    }
}

void MetadataSidebarComponent::showAmbisonicOrientationDialog()
{
    const auto info = engine.getLoadedFileInfo();
    if (!info.isValid() || !info.channelLayout.isAmbisonic)
        return;

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Ambisonic Orientation";
    const juce::Component::SafePointer<MetadataSidebarComponent> safeThis(this);
    options.content.setOwned(new AmbisonicOrientationComponent(
        engine,
        document,
        [safeThis]
        {
            if (safeThis != nullptr)
                safeThis->refreshChannelControls();
        }));
    options.componentToCentreAround = this;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.escapeKeyTriggersCloseButton = true;
    options.dialogBackgroundColour = ModernAppleLookAndFeel::windowBackground();
    options.launchAsync();
}

void MetadataSidebarComponent::paint(juce::Graphics& g)
{
    if (!RendererVisualStyle::isClassic() && !bnkDocument.getBankInfo().isValid())
    {
        ModernPanelPainter::paint(g, getLocalBounds(), "File information", ModernPanelPainter::Icon::information);
        return;
    }
    auto bounds = getLocalBounds().toFloat();
    // Shadow: rgba(0, 0, 0, 0.04), offset-y: 4px, blur-radius: 20px
    g.setColour(juce::Colours::black.withAlpha(0.04f));
    g.fillRoundedRectangle(bounds.translated(0.0f, 4.0f).reduced(0.5f), ModernAppleLookAndFeel::cardRadius() + 1.0f);
    g.setColour(ModernAppleLookAndFeel::panelBackground());
    g.fillRoundedRectangle(bounds.reduced(0.5f), ModernAppleLookAndFeel::cardRadius());

    auto content = getLocalBounds().reduced(24, 26);
    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    g.drawText("Metadata", content.removeFromTop(22), juce::Justification::centredLeft);
    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawText("SELECTION DETAILS", content.removeFromTop(14), juce::Justification::centredLeft);

    content.removeFromTop(8);

    const auto info = engine.getLoadedFileInfo();
    const auto bankInfo = bnkDocument.getBankInfo();

    if (bankInfo.isValid())
        return; // The bank's measured rows are painted by its scrollable child.

    if (!info.isValid())
    {
        drawInspectorItems(g, content, {{"Status", "Metadata appears here after opening a source."}}, 34);
        return;
    }

    drawSectionTitle(g, content, "Channel Monitor");
    content.removeFromTop(info.channelLayout.isAmbisonic ? 253 : 179);
    if (typedMetadataInspector == nullptr)
    {
        drawInspectorItems(g, content,
            {{"Typed Facts", "Available immediately after CoreReady; no second reader is opened."}}, 34);
    }
}

void MetadataSidebarComponent::refreshBankInspector()
{
    const auto bankInfo = bnkDocument.getBankInfo();
    std::vector<InspectorItem> items;
    const auto append = [&items] (std::initializer_list<InspectorItem> values)
    {
        for (const auto& value : values)
            if (value.value.isNotEmpty()) items.push_back(value);
    };

    if (bankInfo.isValid())
    {
        append(
                           {
                               {"Path", bankInfo.file.getFullPathName()},
                               {"Format", "Wwise BNK"},
                               {"Version", juce::String(bankInfo.version)},
                               {"Bank ID", bankInfo.bankId != 0
                                               ? "0x" + juce::String::toHexString(static_cast<juce::int64>(bankInfo.bankId)).toUpperCase()
                                               : "--"},
                               {"Events", juce::String(bankInfo.eventCount)},
                               {"Media", juce::String(bankInfo.mediaCount)},
                               {"HIRC Objects", juce::String(bankInfo.hircObjectCount)}
                           });

        if (const auto selectedGraph = bnkDocument.getSelectedEventGraph(); selectedGraph != nullptr)
        {
            items.push_back({ "Event", {} });

            juce::String playbackSummary;
            juce::String playbackHint;
            juce::String targetObject;
            juce::String mediaCandidates;
            juce::String linkedMedia;
            juce::String stateModifiers;
            juce::String selectedSwitch;
            juce::String selectedState;
            juce::String rtpc;

            if (const auto selectedEventId = bnkDocument.getSelectedEventId(); selectedEventId.has_value())
            {
                if (const auto state = bnkDocument.getState(); state != nullptr)
                {
                    const auto found = std::find_if(state->events.begin(),
                                                    state->events.end(),
                                                    [selectedEventId](const BnkDocument::EventDescriptor& event)
                                                    {
                                                        return event.id == *selectedEventId;
                                                    });
                    if (found != state->events.end())
                    {
                        playbackSummary = found->playbackSummary;
                        playbackHint = BnkDocument::describePlaybackHint(found->playbackHint);
                        targetObject = found->targetObjectName;
                        mediaCandidates = juce::String(static_cast<int>(found->media.size()));
                        const auto linkedMediaNames = bnkDocument.getSelectedEventMediaNames(currentBnkSwitchToken);
                        if (!linkedMediaNames.isEmpty())
                            linkedMedia = linkedMediaNames.joinIntoString(", ");

                        if (!found->stateModifiers.empty())
                        {
                            juce::StringArray stateNames;
                            for (const auto& modifier : found->stateModifiers)
                            {
                                if (modifier.stateName.isNotEmpty())
                                    stateNames.addIfNotAlreadyThere(modifier.stateName
                                                                + (modifier.hasVolume ? " (" + juce::String(modifier.volumeDb, 1) + " dB)" : ""));
                            }
                            if (!stateNames.isEmpty())
                                stateModifiers = stateNames.joinIntoString(", ");
                        }

                        selectedSwitch = currentBnkSwitchLabel;
                        selectedState = currentBnkStateLabel;
                        rtpc = currentBnkRtpcLabel;
                    }
                }
            }

            append(
                               {
                                   {"Selected Event", selectedGraph->eventName},
                                   {"Playback Summary", playbackSummary},
                                   {"Playback Hint", playbackHint},
                                   {"Target Object", targetObject},
                                   {"Media Candidates", mediaCandidates},
                                   {"Linked Media", linkedMedia},
                                   {"State Modifiers", stateModifiers},
                                   {"Selected Switch", selectedSwitch},
                                   {"Selected State", selectedState},
                                   {"RTPC", rtpc},
                                   {"Runtime Graph", juce::String(selectedGraph->nodes.size()) + " nodes"},
                                   {"Sound Nodes", juce::String(selectedGraph->soundNodeCount)}
                               });

            if (selectedGraph->diagnostics.size() > 0)
            {
                items.push_back({ "Resolver Notes", {} });
                for (const auto& note : selectedGraph->diagnostics)
                    append({ { {}, note } });
            }
        }
        else
        {
            append({{"Selected Event", "Choose an event to inspect its routing."}});
        }

        if (const auto selectedMediaId = bnkDocument.getSelectedMediaId(); selectedMediaId.has_value())
        {
            const auto mediaEntries = bnkDocument.getPlayableMedia();
            const auto mediaIt = std::find_if(mediaEntries.begin(),
                                              mediaEntries.end(),
                                              [selectedMediaId](const BnkDocument::PlayableMediaDescriptor& media)
                                              {
                                                  return media.mediaId == *selectedMediaId;
                                              });
            if (mediaIt != mediaEntries.end())
            {
                items.push_back({ "Media", {} });
                juce::String referencedBy;
                const auto referencingEvents = bnkDocument.getEventNamesForMedia(mediaIt->mediaId);
                if (!referencingEvents.isEmpty())
                    referencedBy = referencingEvents.joinIntoString(", ");

                const auto extractedFile = bnkDocument.getEmbeddedMediaCacheFile(mediaIt->mediaId);
                append(
                                   {
                                       {"Selected Media", mediaIt->displayName},
                                       {"Media ID", "0x" + juce::String::toHexString(static_cast<juce::int64>(mediaIt->mediaId)).toUpperCase()},
                                       {"Availability", BnkDocument::describeAvailability(mediaIt->availability)},
                                       {"Packed Media", mediaIt->mediaFile.existsAsFile() ? mediaIt->mediaFile.getFullPathName() : juce::String()},
                                       {"Source Media", mediaIt->sourceFile.existsAsFile() ? mediaIt->sourceFile.getFullPathName() : juce::String()},
                                       {"Extracted Cache", extractedFile.existsAsFile() ? extractedFile.getFullPathName() : juce::String()},
                                       {"Referenced By", referencedBy}
                                   });
            }
        }

        if (bankInfo.diagnostics.size() > 0)
        {
            items.push_back({ "Notes", {} });
            for (const auto& note : bankInfo.diagnostics)
                append({ { {}, note } });
        }
    }
    bankInspectorContent->setItems(std::move(items));
}

void MetadataSidebarComponent::resized()
{
    const bool bankActive = bnkDocument.getBankInfo().isValid();
    if (bankActive)
    {
        if (bankInspectorContent == nullptr)
        {
            bankInspectorContent = std::make_unique<BankInspectorContent>();
            bankInspectorViewport = std::make_unique<juce::Viewport>();
            bankInspectorViewport->setComponentID("bank.metadata.viewport");
            bankInspectorViewport->setTitle("Bank metadata");
            bankInspectorViewport->setWantsKeyboardFocus(true);
            bankInspectorViewport->setViewedComponent(bankInspectorContent.get(), false);
            bankInspectorViewport->setScrollBarsShown(true, false);
            bankInspectorViewport->setScrollBarThickness(8);
            addAndMakeVisible(*bankInspectorViewport);
        }
        refreshBankInspector();
        bankInspectorViewport->setBounds(getLocalBounds().withTrimmedTop(70).reduced(16, 8));
        bankInspectorContent->layoutForWidth(juce::jmax(1,
            bankInspectorViewport->getWidth() - bankInspectorViewport->getScrollBarThickness() - 4));
    }
    else
    {
        bankInspectorViewport.reset();
        bankInspectorContent.reset();
    }
    const bool modernOrdinary = !RendererVisualStyle::isClassic() && !bankActive;
    modernViewport.setVisible(modernOrdinary);
    if (modernOrdinary)
    {
        layoutModernInspector();
        return;
    }
    auto content = getLocalBounds().reduced(24, 26);
    content.removeFromTop(22);
    content.removeFromTop(14);
    content.removeFromTop(8);

    const auto info = engine.getLoadedFileInfo();
    const auto showControls = info.isValid() && !bnkDocument.getBankInfo().isValid();
    channelOrderBox.setVisible(showControls);
    monitorModeBox.setVisible(showControls);
    pingRoleBox.setVisible(showControls);
    pingRoleButton.setVisible(showControls);
    includeLfeToggle.setVisible(showControls);
    outputMappingButton.setVisible(showControls);
    if (ambisonicMonitorModeBox != nullptr)
        ambisonicMonitorModeBox->setVisible(showControls && info.channelLayout.isAmbisonic);
    if (ambisonicOrientationButton != nullptr)
        ambisonicOrientationButton->setVisible(showControls && info.channelLayout.isAmbisonic);
    if (typedMetadataInspector != nullptr)
        typedMetadataInspector->setVisible(showControls);

    if (!showControls)
        return;

    content.removeFromTop(18);
    channelOrderBox.setBounds(content.removeFromTop(30));
    content.removeFromTop(7);
    monitorModeBox.setBounds(content.removeFromTop(30));
    if (info.channelLayout.isAmbisonic && ambisonicMonitorModeBox != nullptr)
    {
        content.removeFromTop(7);
        ambisonicMonitorModeBox->setBounds(content.removeFromTop(30));
    }
    content.removeFromTop(7);
    auto pingRow = content.removeFromTop(30);
    pingRoleButton.setBounds(pingRow.removeFromRight(54));
    pingRow.removeFromRight(8);
    pingRoleBox.setBounds(pingRow);
    content.removeFromTop(7);
    includeLfeToggle.setBounds(content.removeFromTop(26));
    content.removeFromTop(7);
    outputMappingButton.setBounds(content.removeFromTop(30));
    if (info.channelLayout.isAmbisonic && ambisonicOrientationButton != nullptr)
    {
        content.removeFromTop(7);
        ambisonicOrientationButton->setBounds(content.removeFromTop(30));
    }

    if (typedMetadataInspector != nullptr)
    {
        auto inspectorArea = getLocalBounds().reduced(24, 26);
        inspectorArea.removeFromTop(22 + 14 + 8);
        inspectorArea.removeFromTop(info.channelLayout.isAmbisonic ? 271 : 197);
        typedMetadataInspector->setBounds(inspectorArea);
    }
}

void MetadataSidebarComponent::refreshChannelControls()
{
    if (pendingBinauralState && pendingBinauralState != document.getState())
    {
        binauralPreparation.reset();
        pendingBinauralState.reset();
    }
    // Source controls follow the synchronous document state. Engine listeners
    // receive that state asynchronously and may still describe the old order.
    const auto sourceState = document.getState();
    const auto& info = sourceState->info;
    suppressChannelCallbacks = true;
    channelOrderBox.clear(juce::dontSendNotification);
    pingRoleBox.clear(juce::dontSendNotification);
    channelOrderPresetIds.clear();

    if (info.isValid() && !info.channelLayout.candidates.empty())
    {
        auto itemId = 1;
        auto selectedIndex = -1;
        for (const auto& preset : info.channelLayout.candidates)
        {
            channelOrderPresetIds.add(preset.id);
            channelOrderBox.addItem(preset.displayName, itemId++);
            if (preset.id == info.channelLayout.selectedPresetId)
                selectedIndex = channelOrderPresetIds.size() - 1;
        }

        channelOrderBox.setEnabled(true);
        if (selectedIndex >= 0)
            channelOrderBox.setSelectedItemIndex(selectedIndex, juce::dontSendNotification);

        for (const auto role : info.channelLayout.selectedRoles)
            pingRoleBox.addItem(ChannelLayoutCatalog::roleToAbbreviation(role), static_cast<int>(role) + 1);
        const auto canPingSpeaker = pingRoleBox.getNumItems() > 0
                                 && !info.channelLayout.isAmbisonic;
        pingRoleBox.setEnabled(canPingSpeaker);
        pingRoleButton.setEnabled(canPingSpeaker);
        if (ambisonicPingTooltipActive != info.channelLayout.isAmbisonic)
        {
            ambisonicPingTooltipActive = info.channelLayout.isAmbisonic;
            pingRoleBox.setTooltip(ambisonicPingTooltipActive
                ? "Ambisonic ACN components are sound-field components, not physical speakers."
                : "Choose a speaker role to ping through the current monitor routing.");
            pingRoleButton.setTooltip(ambisonicPingTooltipActive
                ? "Use output-matrix speaker ping to verify physical monitor routing."
                : "Play a short low-level speaker test signal for the selected role.");
        }
        if (pingRoleBox.getNumItems() > 0)
            pingRoleBox.setSelectedItemIndex(0, juce::dontSendNotification);
    }
    else
    {
        channelOrderBox.addItem("No channel order", 1);
        channelOrderBox.setSelectedItemIndex(0, juce::dontSendNotification);
        channelOrderBox.setEnabled(false);
        pingRoleBox.addItem("--", 1);
        pingRoleBox.setSelectedItemIndex(0, juce::dontSendNotification);
        pingRoleBox.setEnabled(false);
        pingRoleButton.setEnabled(false);
    }

    monitorModeBox.setSelectedId(static_cast<int>(engine.getMonitorMode()) + 1, juce::dontSendNotification);
    monitorModeBox.setEnabled(info.isValid());
    includeLfeToggle.setToggleState(engine.shouldIncludeLfeInDownmix(), juce::dontSendNotification);
    outputMappingButton.setEnabled(info.isValid() && info.numChannels > 2);
    outputMappingButton.setButtonText(engine.getOutputMapping().enabled
                                          ? "Output mapping (custom)..."
                                          : "Output mapping...");
    if (info.channelLayout.isAmbisonic)
    {
        if (ambisonicMonitorModeBox == nullptr)
        {
            ambisonicMonitorModeBox = std::make_unique<juce::ComboBox>();
            ambisonicMonitorModeBox->setComponentID("ambisonic-monitor-mode");
            ambisonicMonitorModeBox->setName("Ambisonic monitor mode");
            ambisonicMonitorModeBox->addItem("Speaker decode", 1);
            ambisonicMonitorModeBox->addItem("Binaural / HRTF", 2);
            ambisonicMonitorModeBox->setTooltip(
                "Speaker decode follows the output device. Binaural loads the SADIE HRTF resource on demand.");
            ambisonicMonitorModeBox->onChange = [this]
            {
                if (suppressChannelCallbacks || ambisonicMonitorModeBox == nullptr)
                    return;

                const auto mode = ambisonicMonitorModeBox->getSelectedId() == 2
                                ? AmbisonicMonitorMode::binauralHrtf
                                : AmbisonicMonitorMode::speakerProjection;
                if (mode == AmbisonicMonitorMode::binauralHrtf)
                {
                    requestBinauralMonitor();
                    return;
                }
                binauralPreparation.reset();
                pendingBinauralState.reset();
                document.setAmbisonicMonitorMode(mode);
                refreshChannelControls();
            };
            if (RendererVisualStyle::isClassic()) addAndMakeVisible(*ambisonicMonitorModeBox);
            else modernContent.addAndMakeVisible(*ambisonicMonitorModeBox);
        }
        ambisonicMonitorModeBox->changeItemText(2, binauralPreparation ? "Preparing HRTF..." : "Binaural / HRTF");
        ambisonicMonitorModeBox->setSelectedId(
            binauralPreparation || document.getAmbisonicMonitorMode() == AmbisonicMonitorMode::binauralHrtf ? 2 : 1,
            juce::dontSendNotification);
        ambisonicMonitorModeBox->setEnabled(
            AmbisonicMonitorDecoder::statusFor(info.channelLayout)
                == AmbisonicMonitorDecodeStatus::ready);

        if (ambisonicOrientationButton == nullptr)
        {
            ambisonicOrientationButton = std::make_unique<juce::TextButton>();
            ambisonicOrientationButton->setTooltip(
                "Rotate the decoded Ambisonic sound field. Controls are created only when opened.");
            ambisonicOrientationButton->onClick = [this]
            {
                showAmbisonicOrientationDialog();
            };
            if (RendererVisualStyle::isClassic()) addAndMakeVisible(*ambisonicOrientationButton);
            else modernContent.addAndMakeVisible(*ambisonicOrientationButton);
        }

        const auto orientation = document.getAmbisonicOrientation();
        ambisonicOrientationButton->setEnabled(true);
        ambisonicOrientationButton->setButtonText(
            "Orientation  Y " + juce::String(orientation.yawDegrees, 0)
            + "  P " + juce::String(orientation.pitchDegrees, 0)
            + "  R " + juce::String(orientation.rollDegrees, 0) + "...");
    }
    else
    {
        ambisonicMonitorModeBox.reset();
        ambisonicOrientationButton.reset();
    }
    suppressChannelCallbacks = false;
    resized();
    repaint();
}

void MetadataSidebarComponent::layoutModernInspector()
{
    modernViewport.setBounds(getLocalBounds().withTrimmedTop(ModernPanelPainter::headerHeight).reduced(2, 3));
    const auto width = juce::jmax(1, modernViewport.getWidth() - modernViewport.getScrollBarThickness());
    const auto info = engine.getLoadedFileInfo();
    const bool available = info.isValid();
    fileSummary.setAvailableHeight(modernViewport.getHeight());
    fileSummary.setBounds(0, 0, width, 152);
    fileSummary.setSize(width, fileSummary.preferredHeight());
    int y = fileSummary.getBottom();
    const auto row = [&y, width] (int height)
    {
        const auto bounds = juce::Rectangle<int>(10, y, juce::jmax(1, width - 20), height);
        y += height + 5;
        return bounds;
    };
    monitorDisclosure.setButtonText(monitorDisclosure.getToggleState() ? "Hide channel monitor" : "Channel monitor...");
    detailsDisclosure.setButtonText(detailsDisclosure.getToggleState() ? "Hide detailed metadata" : "Detailed metadata...");
    monitorDisclosure.setEnabled(available);
    detailsDisclosure.setEnabled(typedMetadataInspector != nullptr);
    monitorDisclosure.setBounds(row(27));
    const bool showMonitor = available && monitorDisclosure.getToggleState();
    for (auto* c : std::array<juce::Component*, 6> { &channelOrderBox, &monitorModeBox, &pingRoleBox,
            &pingRoleButton, &includeLfeToggle, &outputMappingButton }) c->setVisible(showMonitor);
    if (ambisonicMonitorModeBox != nullptr) ambisonicMonitorModeBox->setVisible(showMonitor && info.channelLayout.isAmbisonic);
    if (ambisonicOrientationButton != nullptr) ambisonicOrientationButton->setVisible(showMonitor && info.channelLayout.isAmbisonic);
    if (showMonitor)
    {
        channelOrderBox.setBounds(row(28));
        monitorModeBox.setBounds(row(28));
        if (ambisonicMonitorModeBox != nullptr && info.channelLayout.isAmbisonic)
            ambisonicMonitorModeBox->setBounds(row(28));
        auto ping = row(28);
        pingRoleButton.setBounds(ping.removeFromRight(48));
        ping.removeFromRight(6);
        pingRoleBox.setBounds(ping);
        includeLfeToggle.setButtonText(width < 280 ? "Include LFE" : "Include LFE in stereo downmix");
        includeLfeToggle.setBounds(row(26));
        outputMappingButton.setBounds(row(28));
        if (ambisonicOrientationButton != nullptr && info.channelLayout.isAmbisonic)
            ambisonicOrientationButton->setBounds(row(28));
    }
    detailsDisclosure.setBounds(row(27));
    if (typedMetadataInspector != nullptr)
    {
        typedMetadataInspector->setVisible(detailsDisclosure.getToggleState());
        if (detailsDisclosure.getToggleState())
            typedMetadataInspector->setBounds(row(juce::jmax(juce::jlimit(260, 480, getHeight() - 200),
                TypedMetadataInspectorComponent::preferredModernHeight(juce::jmax(1, width - 20)))));
    }
    modernContent.setSize(width, juce::jmax(y + 4, modernViewport.getHeight()));
}
} // namespace singleeditor
