#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <atomic>
#include <vector>

juce::ApplicationProperties& getAppProperties();

class PreferencesWindow;

class IconMenu final : public juce::SystemTrayIconComponent,
                       private juce::Timer,
                       public juce::ChangeListener
{
public:
    IconMenu();
    ~IconMenu() override;

    void mouseDown (const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster* changed) override;

    [[nodiscard]] static juce::String getKey (const juce::String& type, const juce::PluginDescription& plugin);

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
    void removePluginsLackingInputOutput();
    void handleDeletePlugin (int index);
    void handleBypassPlugin (int index);
    void handleEditPlugin (int index);
    void handleMovePlugin (int index, bool moveUp);

    [[nodiscard]] const std::vector<juce::PluginDescription>& getTimeSortedList() const;
    void reconnectGraph();
    void autoMatchSampleRate();

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
    bool isHandlingDeviceChange = false;

    // Background plugin loading — instances created on a worker thread,
    // marshalled back to the message thread one at a time via callAsync.
    class PluginLoadThread;
    std::unique_ptr<PluginLoadThread> pluginLoadThread;
    std::atomic<int> pluginLoadGeneration { 0 };

    void onPluginInstanceReady (std::unique_ptr<juce::AudioProcessor> instance,
                                juce::AudioProcessorGraph::NodeID nodeId,
                                int generation,
                                juce::String savedState);
    void onAllPluginsLoaded (int generation);

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;

    std::unique_ptr<PreferencesWindow> preferencesWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconMenu)
};
