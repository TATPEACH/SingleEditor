#pragma once
#include "Bnk/BnkDocument.h"

namespace singleeditor::bnkstate
{
// Shared by admitted bank metadata, controls and preview requests. Keep these
// operations independent of the decoder's lifetime and translation unit.
juce::String id(const juce::String& value);
juce::String name(const juce::String& value);
juce::String identity(const juce::String& token, uint32_t shortId, const juce::String& label);
juce::String group(const BnkDocument::StateValueDescriptor& value);
juce::String group(const BnkDocument::EventDescriptor::StateModifier& value);
juce::String token(const BnkDocument::StateValueDescriptor& value);
bool matches(const BnkDocument::EventDescriptor::StateModifier& modifier,
             const BnkDocument::StateValueDescriptor& value);
std::vector<BnkDocument::StateValueDescriptor> options(
    const BnkDocument::State& state, const BnkDocument::EventDescriptor& event);
}
