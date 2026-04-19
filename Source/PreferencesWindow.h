#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>
#include <vector>

//==============================================================================
// Preferences window — unified signal-flow view.
//
// Layout (top to bottom): INPUT → AUDIO CHAIN → OUTPUT → DEVICE SETTINGS.
// Plugin chain changes are staged locally until the user clicks Apply.
// Bypass state (checkbox per plugin) is also staged and delivered via onApply.
// Clicking Apply commits changes and keeps the window open for further editing.
// Close (with or without Apply) calls onClose so the owner can persist audio
// device state and release the window pointer.
//==============================================================================
class PreferencesWindow final : public juce::DocumentWindow
{
public:
    PreferencesWindow (
        juce::AudioDeviceManager& deviceManager,
        juce::KnownPluginList& knownPlugins,
        const std::vector<juce::PluginDescription>& activeChain,
        const std::vector<bool>& bypassStates,
        std::function<void (const std::vector<juce::PluginDescription>&,
                            const std::vector<bool>&)> onApply,
        std::function<void (const juce::PluginDescription&)> onEditPlugin,
        std::function<void()> onClose);

    ~PreferencesWindow() override;
    void closeButtonPressed() override;

    void refreshPluginChain (const std::vector<juce::PluginDescription>& chain,
                             const std::vector<bool>& bypassStates);

private:
    std::function<void()> onCloseFn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesWindow)
};
