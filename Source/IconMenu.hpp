#pragma once

#include "StatusSink.hpp"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <set>
#include <vector>

juce::ApplicationProperties& getAppProperties();

class PreferencesWindow;

class IconMenu final : public juce::SystemTrayIconComponent,
                       private juce::Timer,
                       private juce::AudioProcessorListener,
                       private juce::AsyncUpdater,
                       public juce::ChangeListener
{
public:
    IconMenu();
    ~IconMenu() override;

    void mouseDown (const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster* changed) override;

    // Menu action ID offsets — each plugin gets an ID in the range [offset, offset + maxPlugins)
    static constexpr int kEditOffset     = 1'000'000;
    static constexpr int kBypassOffset   = 2'000'000;
    static constexpr int kDeleteOffset   = 3'000'000;
    static constexpr int kMoveUpOffset   = 4'000'000;
    static constexpr int kMoveDownOffset = 5'000'000;

private:
    using NodeID = juce::AudioProcessorGraph::NodeID;

    void timerCallback() override;
    void reloadPlugins();
    void showPreferences();
    void applyPluginChain (const std::vector<juce::PluginDescription>& newChain,
                           const std::vector<bool>& bypassStates,
                           const std::vector<int>& laneStates);
    void openPluginEditorFor (const juce::PluginDescription& pd);
    void refreshPreferencesIfOpen();
    void loadActivePlugins();
    void cancelPluginLoading();
    void savePluginStates();
    void deletePluginStates();
    void setIcon();
    void handleDeletePlugin (int index);
    void handleBypassPlugin (int index);
    void handleEditPlugin (int index);
    void handleMovePlugin (int index, bool moveUp);

    /** Saves the settings file and reports a failure rather than discarding it. */
    void flushSettings (juce::PropertiesFile& settings, const juce::String& context);

    /** Sets the tray tooltip from the current state: loading, a problem, or idle. */
    void refreshTooltip();

    [[nodiscard]] const std::vector<juce::PluginDescription>& getTimeSortedList() const;
    void reconnectGraph();
    void autoMatchSampleRate();
    void logAudioConfig (const juce::String& contextLabel) const;

    // Watching hosted plugins for changes that invalidate the routing.
    //
    // juce::AudioProcessorGraph does not listen to its own nodes: its render
    // sequence records each node's reported latency when it is built, and nothing
    // rebuilds it when a plugin changes that later. A plugin that switches to a
    // linear-phase or oversampled mode inside its own editor therefore leaves
    // every other lane compensated by the old amount, which is a quiet phase
    // error rather than an obvious failure.
    //
    // audioProcessorChanged can arrive on any thread, so the response is deferred
    // through AsyncUpdater rather than acted on in place: triggerAsyncUpdate is
    // safe to call from anywhere, whereas rewiring the graph is not, and building
    // a Component::SafePointer off the message thread is not either.
    void audioProcessorChanged (juce::AudioProcessor* processor,
                                const ChangeDetails& details) override;
    void audioProcessorParameterChanged (juce::AudioProcessor* processor,
                                         int parameterIndex,
                                         float newValue) override;
    void handleAsyncUpdate() override;

    void listenTo (juce::AudioProcessor& processor);
    void stopListeningTo (NodeID nodeId);

    #if JUCE_WINDOWS
    [[nodiscard]] static juce::File getStartupShortcutPath();
    [[nodiscard]] bool isStartupEnabled() const;
    void setStartupEnabled (bool shouldEnable);
    #endif

    static void menuInvocationCallback (int id, IconMenu*);

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;
    juce::KnownPluginList activePluginList;
    juce::KnownPluginList::SortMethod pluginSortMethod = juce::KnownPluginList::sortByManufacturer;
    juce::PopupMenu menu;
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    NodeID inputNodeId;
    NodeID outputNodeId;

    mutable std::vector<juce::PluginDescription> sortedPluginCache;
    mutable bool sortedCacheDirty = true;

    // Stops a *synchronous* re-entry into the device-change handler. It cannot
    // stop the recursion that actually happens: change broadcasts are async, so
    // the second callback arrives after this has already been cleared. What bounds
    // that is sampleRateCorrections, see Source/SampleRatePolicy.hpp.
    bool isHandlingDeviceChange = false;

    // Corrective sample-rate requests made since the device last reported a rate
    // it supports. Reset the moment it settles.
    int sampleRateCorrections = 0;

    // Where failures go so the user can see them, rather than only the log file.
    lighthost::status::Sink status;

    // Background plugin loading. Plugin instantiation must happen on the message
    // thread — JUCE loads the DLL and calls the plugin factory there no matter
    // which thread asks — so this is a message-thread chain: load one plugin, and
    // on its async completion callback start the next. There is no worker thread,
    // so nothing can deadlock when a load is cancelled mid-flight.
    //
    // Cancellation is a generation bump: a callback from a superseded load sees a
    // newer generation and drops its result. Everything here is message-thread
    // only, so no atomics are needed.
    struct LoadSpec
    {
        juce::PluginDescription description;
        juce::String            savedState;
        juce::uint32            nodeId;
    };

    std::vector<LoadSpec> pendingLoads;
    size_t                nextLoadIndex = 0;
    int                   pluginLoadGeneration = 0;

    // NodeIDs whose saved state threw or decoded to nothing while restoring.
    // savePluginStates() skips these, so a one-off restore failure cannot
    // overwrite a good saved blob with the plugin's factory defaults.
    std::set<juce::uint32> statesNotRestored;

    void loadNextPlugin();
    void onPluginInstanceReady (std::unique_ptr<juce::AudioProcessor> instance,
                                juce::AudioProcessorGraph::NodeID nodeId,
                                int generation,
                                const juce::String& savedState);
    void restorePluginState (juce::AudioProcessorGraph::Node& node,
                             juce::AudioProcessorGraph::NodeID nodeId,
                             const juce::String& savedState);
    void onAllPluginsLoaded (int generation);

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;

    std::unique_ptr<PreferencesWindow> preferencesWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconMenu)
};
