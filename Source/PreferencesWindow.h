#pragma once

#include "Lanes.hpp"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <functional>
#include <vector>

//==============================================================================
// Preferences window — unified signal-flow view.
//
// Layout (top to bottom): INPUT → AUDIO CHAIN → LANE TRIM → OUTPUT →
// DEVICE SETTINGS.
//
// Plugin chain changes are staged locally until the user clicks Apply.
// Bypass state (checkbox per plugin) is also staged and delivered via onApply.
// Clicking Apply commits changes and keeps the window open for further editing.
// Close (with or without Apply) calls onClose so the owner can persist audio
// device state and release the window pointer.
//
// Lane trims are the exception to staging: a level control you cannot hear while
// dragging is not a level control, so they apply immediately and are written to
// disk when the gesture ends.
//==============================================================================
class PreferencesWindow final : public juce::DocumentWindow
{
public:
    /** The lane trims: where they start, and where changes go.

        onChanged fires continuously while a trim is moved and must be cheap: it
        is meant to reach the running graph and nothing else. onCommitted fires
        once a gesture is finished and is where the value is persisted, because
        each write rewrites the whole settings document.
    */
    struct LaneTrim
    {
        std::array<float, lighthost::kNumLanes> initialDb {};
        std::function<void (int lane, float decibels)> onChanged;
        std::function<void (int lane, float decibels)> onCommitted;
    };

    PreferencesWindow (
        juce::AudioDeviceManager& deviceManager,
        juce::KnownPluginList& knownPlugins,
        const std::vector<juce::PluginDescription>& activeChain,
        const std::vector<bool>& bypassStates,
        const std::vector<int>& laneStates,
        LaneTrim laneTrim,
        std::function<void (const std::vector<juce::PluginDescription>&,
                            const std::vector<bool>&,
                            const std::vector<int>&)> onApply,
        std::function<void (const juce::PluginDescription&)> onEditPlugin,
        /** Total latency the graph declares, in samples. Read for display only:
            plugin latency is inherent to the plugins and nothing the host can
            compensate on a live monitoring path. */
        std::function<int()> chainLatencySamples,
        std::function<void()> onClose);

    ~PreferencesWindow() override;
    void closeButtonPressed() override;

    void refreshPluginChain (const std::vector<juce::PluginDescription>& chain,
                             const std::vector<bool>& bypassStates,
                             const std::vector<int>& laneStates);

    /** Shows the most recent problem at the top of the panel, or hides the row
        when passed an empty string. The tray tooltip carries the same message,
        but a tooltip is only found by someone already suspicious.
    */
    void setStatusMessage (const juce::String& message);

    /** Shows a short-lived confirmation beside the Apply button saying what the
        click actually did.

        Applying was silent on success and silent on a no-op, so the two were
        indistinguishable. This is transient by design: a confirmation that stays
        on screen stops being a confirmation and becomes furniture.
    */
    void setApplyFeedback (const juce::String& message);

private:
    std::function<void()> onCloseFn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesWindow)
};
