#pragma once
#include "Audio/AsioSettingsSession.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace singleeditor
{
class AsioSettingsComponent final : public juce::Component,
                                    private juce::ChangeListener,
                                    private juce::TableListBoxModel
{
public:
    explicit AsioSettingsComponent(std::shared_ptr<AsioSettingsSession>);
    ~AsioSettingsComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void refresh();
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int, int, int, bool) override;
    void paintCell(juce::Graphics&, int, int, int, int, bool) override;
    juce::Component* refreshComponentForCell(int, int, bool, juce::Component*) override;
    std::shared_ptr<AsioSettingsSession> session;
    juce::Label title, help, status, liveOutput;
    juce::TextEditor warnings;
    juce::ComboBox backends, drivers, profiles, rates, buffers, pingOutput;
    juce::TextButton windowsDefault {"Choose Windows default"};
    juce::TextButton discover {"Refresh devices"}, inspect {"Read capabilities"}, cancel {"Cancel operation"};
    juce::TextButton validate {"Validate"}, ping {"Ping"}, activate {"Activate"};
    juce::TextButton retrySaved {"Retry output"}, controlPanel {"ASIO Control Panel"};
    juce::ToggleButton confirm {"I confirm the listed routing warnings"};
    juce::ToggleButton followSource {"Follow Source Sample Rate (expert)"};
    juce::TableListBox matrix;
    std::vector<std::string> visibleNames;
    std::uint64_t displayedRevision = 0;
    bool updating = false;
};
}
