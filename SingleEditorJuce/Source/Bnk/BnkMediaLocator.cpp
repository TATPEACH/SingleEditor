#include "Bnk/BnkMediaLocator.h"

namespace singleeditor::bnkmedia
{
// Requires the caller's bounded optional-work scope. Discovery does not grant
// a read lease: the preview request must still freeze the actual chosen input.
juce::File findOriginal(const juce::File& declaredSource, const juce::File& sourceRoot,
    const juce::File& projectRoot, const juce::String& relativePath, const juce::String& shortName)
{
    if (declaredSource != juce::File())
        return declaredSource.existsAsFile() ? declaredSource : juce::File();
    const juce::File roots[] { sourceRoot, projectRoot == juce::File() ? juce::File() : projectRoot.getChildFile("Originals") };
    for (size_t i=0;i<2;++i)
    {
        const auto& root=roots[i];
        if (!BoundedSidecarInput::continueWork()) return {};
        if (!root.isDirectory() || (i==1 && root==roots[0])) continue;
        if (relativePath.isNotEmpty())
        {
            const auto candidate=BoundedSidecarInput::withinRoot(root,relativePath);
            if (candidate.existsAsFile()) return candidate;
        }
        if (shortName.isEmpty()) continue;
        const auto matches=BoundedSidecarInput::files(root,shortName);
        juce::File unique;
        for (const auto& candidate:matches)
        {
            if (!BoundedSidecarInput::continueWork()) return {};
            if (!candidate.getFileName().equalsIgnoreCase(shortName)) continue;
            if (unique != juce::File()) return {}; // Equal names do not identify one source.
            unique=candidate;
        }
        if (unique != juce::File()) return unique;
    }
    return {};
}
}
