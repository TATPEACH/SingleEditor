#pragma once
#include "Bnk/BoundedSidecarInput.h"

namespace singleeditor::bnkmedia
{
// Requires the caller's bounded optional-work scope. Discovery does not grant
// a read lease: the preview request must still freeze the actual chosen input.
juce::File findOriginal(const juce::File& declaredSource, const juce::File& sourceRoot,
    const juce::File& projectRoot, const juce::String& relativePath, const juce::String& shortName);
}
