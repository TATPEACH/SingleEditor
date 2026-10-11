#pragma once

#include <juce_core/juce_core.h>

namespace singleeditor
{
bool readAutomationPlan(const juce::File& source,
                        juce::var& plan,
                        juce::String& errorMessage);

bool writeAutomationPlanAtomically(const juce::File& target,
                                   const juce::var& plan,
                                   juce::String& errorMessage);
} // namespace singleeditor
