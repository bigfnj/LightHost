#pragma once

// Deliberately NOT juce_audio_utils. Nothing here uses a symbol from it -- the
// window needs juce_gui_extra for DocumentWindow and juce_audio_processors for
// GenericAudioProcessorEditor and the graph -- and including it would make the
// test target link juce_audio_devices, with its whole set of platform audio
// backends, to test a window.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

class PluginWindow final : public juce::DocumentWindow
{
public:
    /** Normal asks the plugin for its own editor and falls back to Generic when
        it has none, so those two are the only kinds that can exist.

        There were two more, Programs and Parameters, with an editor class behind
        Programs. Nothing ever passed them: both call sites pass Normal. They are
        gone rather than kept for a caller that never arrived -- the window-position
        property keys below are derived from this enum, so a dead value here is a
        dead settings key too.
    */
    enum WindowFormatType
    {
        Normal = 0,
        Generic
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
        case PluginWindow::Normal:  return "Normal";
        case PluginWindow::Generic: return "Generic";
        default:                    return {};
    }
}

[[nodiscard]] inline juce::String getLastXProp (PluginWindow::WindowFormatType type) { return "uiLastX_" + toString (type); }
[[nodiscard]] inline juce::String getLastYProp (PluginWindow::WindowFormatType type) { return "uiLastY_" + toString (type); }
[[nodiscard]] inline juce::String getOpenProp  (PluginWindow::WindowFormatType type) { return "uiopen_"  + toString (type); }
