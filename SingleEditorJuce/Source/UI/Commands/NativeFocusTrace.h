#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include <typeinfo>
#endif

namespace singleeditor
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
// Opt-in observation only: no input synthesis, timer, focus mutation or startup work.
inline void traceNativeFocus(const char* stage, juce::Component& receiver,
                             const juce::KeyPress* key = nullptr)
{
    static const auto path = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_NATIVE_FOCUS_TRACE", {});
    if (path.isEmpty()) return;
    const auto describe = [](juce::Component* component)
    {
        return component == nullptr ? juce::String("none")
            : juce::String(typeid(*component).name()) + "[" + component->getComponentID()
                + ";" + component->getTitle() + "]";
    };
    const auto line = juce::String(stage)
        + " receiver=" + describe(&receiver)
        + " focus=" + describe(juce::Component::getCurrentlyFocusedComponent())
        + " receiverHasFocus=" + juce::String(receiver.hasKeyboardFocus(true) ? 1 : 0)
        + (key != nullptr ? " key=" + key->getTextDescription()
            + " code=" + juce::String(key->getKeyCode())
            + " flags=" + juce::String(key->getModifiers().getRawFlags()) : juce::String());
    juce::File(path).appendText(line + "\n");
}
#else
inline void traceNativeFocus(const char*, juce::Component&, const juce::KeyPress* = nullptr) noexcept {}
#endif
} // namespace singleeditor
