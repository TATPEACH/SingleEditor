#pragma once

#include <cstdint>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Document/SingleTrackDocument.h"
#include "Metadata/TypedMetadataInspectorModel.h"

namespace singleeditor
{
class TypedMetadataInspectorComponent final : public juce::Component
{
public:
    TypedMetadataInspectorComponent();
    ~TypedMetadataInspectorComponent() override;

    void beginGeneration(std::uint64_t generation);
    bool activatePostCore(std::uint64_t generation,
                          const SingleTrackDocument::FileInfo& info);
    bool refreshFacts(std::uint64_t generation,
                      const SingleTrackDocument::FileInfo& info);
    bool reportOptionalProviderFailure(std::uint64_t generation,
                                       juce::String message);

    [[nodiscard]] bool isActiveFor(std::uint64_t generation) const noexcept;
    [[nodiscard]] static int preferredModernHeight(int width) noexcept;
    [[nodiscard]] MetadataInspectorDensity getDensity() const noexcept { return density; }
    [[nodiscard]] const TypedMetadataInspectorModel* getModelForTesting() const noexcept
    {
        return model.get();
    }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;

private:
    class ContentComponent;

    void applyProjectionSelection();
    void refreshPresentation();

    std::uint64_t expectedGeneration = 0;
    MetadataInspectorDensity density = MetadataInspectorDensity::compact;
    std::unique_ptr<TypedMetadataInspectorModel> model;
    std::unique_ptr<ContentComponent> content;
    juce::Viewport viewport;
    juce::TextEditor searchEditor;
    juce::ComboBox copyScopeBox;
    juce::TextButton copyButton { "Copy" };
    juce::ToggleButton longValuesToggle { "Long" };
    juce::ToggleButton rawToggle { "Raw" };
    juce::ToggleButton provenanceToggle { "Source" };
};
} // namespace singleeditor
