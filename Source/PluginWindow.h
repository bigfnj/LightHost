#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

class PluginWindow final : public juce::DocumentWindow
{
public:
    enum WindowFormatType
    {
        Normal = 0,
        Generic,
        Programs,
        Parameters,
        NumTypes
    };

    PluginWindow (juce::Component* pluginEditor, juce::AudioProcessorGraph::Node::Ptr, WindowFormatType);
    ~PluginWindow() override;

    static PluginWindow* getWindowFor (juce::AudioProcessorGraph::Node::Ptr, WindowFormatType);
    static void closeCurrentlyOpenWindowsFor (juce::AudioProcessorGraph::NodeID nodeId);
    static void closeAllCurrentlyOpenWindows();
    [[nodiscard]] static bool containsActiveWindows();

    void moved() override;
    void closeButtonPressed() override;

private:
    juce::AudioProcessorGraph::Node::Ptr owner;
    WindowFormatType type;

    float getDesktopScaleFactor() const override { return 1.0f; }

    [[nodiscard]] static juce::Array<PluginWindow*>& getActiveWindows();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

[[nodiscard]] inline juce::String toString (PluginWindow::WindowFormatType type)
{
    switch (type)
    {
        case PluginWindow::Normal:     return "Normal";
        case PluginWindow::Generic:    return "Generic";
        case PluginWindow::Programs:   return "Programs";
        case PluginWindow::Parameters: return "Parameters";
        case PluginWindow::NumTypes:   return {};
        default:                       return {};
    }
}

[[nodiscard]] inline juce::String getLastXProp (PluginWindow::WindowFormatType type) { return "uiLastX_" + toString (type); }
[[nodiscard]] inline juce::String getLastYProp (PluginWindow::WindowFormatType type) { return "uiLastY_" + toString (type); }
[[nodiscard]] inline juce::String getOpenProp  (PluginWindow::WindowFormatType type) { return "uiopen_"  + toString (type); }
