#pragma once

// ConfirmPolicy.hpp, DevicePolicy.hpp and Lanes.hpp were here and are now
// included by IconMenu.cpp instead: nothing declared below names a type or a
// constant from any of them. The first two stop being parsed by everything
// that includes this header; Lanes.hpp still arrives through NodeIds.hpp, so
// moving it only puts the include where the use is. NodeIds.hpp itself stays,
// because maxProbes sizes a member array below.
#include "GainProcessor.hpp"
#include "DeviceTap.hpp"
#include "NodeIds.hpp"
#include "PluginStateVault.hpp"
#include "SettingsKeys.hpp"
#include "StatusSink.hpp"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <map>
#include <memory>
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

    /** Opens the Preferences window, as a left-click on the tray icon does.

        Public so the headless self-test can build the window: it is the only
        window this application has, it holds every control, and nothing else
        constructs it during an automated run.
    */
    void showPreferencesWindow();

    /** Reports a message through the same status sink the tray tooltip and the
        Preferences status row already read.

        Public because PluginHostApp needs it: a second launch is handled by the
        application object, outside IconMenu entirely, and it is exactly the kind
        of event the user has to be told about.
    */
    void reportStatus (const juce::String& message);

    /** Records the audio devices the user chose, so that a substitution stays
        detectable after JUCE has overwritten its own record of the request.

        ONLY A DELIBERATE CHOICE MAY CALL THIS. Today that is the Preferences
        Apply path, which is the only place in the application where a device
        name is picked by hand. Anything that writes device state incidentally
        must not: it would store whatever JUCE fell back to as the thing the
        user asked for, and the substitution would be undetectable again --
        exactly the failure this closes. autoMatchSampleRate is the concrete
        example, and it does not call this.

        Takes the combo entries as they stand. A placeholder such as
        "(no input devices)" is dropped rather than recorded, so the caller does
        not have to filter -- see lighthost::device::asRequest.
    */
    void recordRequestedDevices (const juce::String& input, const juce::String& output);

    // Menu action ID bands. Each action gets a band, and a plugin's item within
    // it is `offset + chainIndex`, so the band has to be wider than any chain.
    //
    // The stride is named because the dispatch in menuInvocationCallback needs
    // the WIDTH, and it used to reach for kEditOffset -- which is the right
    // number only because it happens to equal the spacing. The comment here
    // pointed at a "maxPlugins" that has never existed.
    static constexpr int kMenuBandStride = 1'000'000;

    static constexpr int kEditOffset     = 1 * kMenuBandStride;
    static constexpr int kBypassOffset   = 2 * kMenuBandStride;
    static constexpr int kDeleteOffset   = 3 * kMenuBandStride;
    static constexpr int kMoveUpOffset   = 4 * kMenuBandStride;
    static constexpr int kMoveDownOffset = 5 * kMenuBandStride;

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

    // Set when a chain apply started an asynchronous plugin load, so the
    // completion handler can tell an apply apart from the startup load.
    bool applyInitiatedLoad = false;
    /** Asks first, then calls deletePluginStates() if the answer is yes. */
    void confirmDeletePluginStates();
    void deletePluginStates();
    void setIcon();
    void handleDeletePlugin (int index);
    void handleBypassPlugin (int index);
    void handleEditPlugin (int index);
    void handleMovePlugin (int index, bool moveUp);

    // Lane trims. One gain node per lane at a reserved id, at that lane's
    // summing point. See Source/GainProcessor.hpp for why they exist. Said
    // "four" until Lanes.hpp existed to stop exactly that kind of prose copy.
    [[nodiscard]] static NodeID laneGainNodeId (int lane);

    // Metering probes, on reserved ids above the lane trims. Bounded so the
    // reserved range cannot run into anything else; a chain longer than this
    // still works, it just stops being probed past the limit.
    static constexpr int kMaxProbes = lighthost::nodeids::maxProbes;
    [[nodiscard]] static NodeID probeNodeId (int index);
    void syncProbeNodes();
    bool signalViewEnabled = false;

    // The probe meters live HERE, not inside the Probe processors, because the
    // graph destroys every processor on a chain reload while the signal view is
    // still holding pointers to them. Owning them at this level makes them as
    // long-lived as the device meters, which is what makes handing a raw pointer
    // to the UI safe.
    std::array<lighthost::metering::Meter, lighthost::nodeids::maxProbes> probeMeters;
    void createLaneGainNodes();
    [[nodiscard]] lighthost::gain::Processor* laneGainProcessor (int lane);

public:
    /** Turns the per-plugin signal view on or off.

        Probes are created only while it is on, so the graph a user runs all day
        is exactly what it was before this feature existed. Costs one graph
        rebuild, which the diffing in reconnectGraph makes inaudible.
    */
    void setSignalViewEnabled (bool shouldBeEnabled);

    /** The meter for the probe sitting after chain position `index`, or nullptr
        when the signal view is off or that position has no probe.
    */
    [[nodiscard]] lighthost::metering::Meter* getProbeMeter (int index);

    /** The device meters, for the Preferences UI. Both outlive any Preferences
        window, because IconMenu owns it.
    */
    [[nodiscard]] lighthost::metering::Meter& getInputMeter()  noexcept { return deviceTap.getInputMeter(); }
    [[nodiscard]] lighthost::metering::Meter& getOutputMeter() noexcept { return deviceTap.getOutputMeter(); }

    /** The stored trim for a lane, in decibels. For the Preferences UI. */
    [[nodiscard]] float getLaneGainDb (int lane) const;

    /** Applies a trim to the running graph without persisting it. */
    void setLaneGainDb (int lane, float decibels);

    /** Writes a trim to the settings file. Called when a drag ends, not per pixel. */
    void persistLaneGainDb (int lane, float decibels);

private:
    /** Saves the settings file and reports a failure rather than discarding it. */
    void flushSettings (juce::PropertiesFile& settings, const juce::String& context);

    /** Sets the tray tooltip from the current state: loading, a problem, or idle. */
    void refreshTooltip();

    /** The chain in display order.

        Returns a SNAPSHOT the caller owns, not a reference to the cache. It used
        to return the reference, and every caller that outlived a rebuild had to
        know to copy the element it wanted out -- three of them carried a comment
        saying so. Two others were safe only because of the order statements
        happened to be in: reconnectGraph iterates it in a range-for, and stays
        correct only because syncProbeNodes rebuilds the cache earlier in the
        same function, while savePluginStates holds it across file writes and a
        status report that is one call away from rebuilding it. Moving either
        would have been undefined behaviour with nothing to catch it.

        Cheap to hold: a refcount bump, where four call sites previously deep
        copied the whole vector on every Preferences open and refresh.
    */
    using ChainSnapshot = std::shared_ptr<const std::vector<juce::PluginDescription>>;
    [[nodiscard]] ChainSnapshot getTimeSortedList() const;
    void reconnectGraph();
    void autoMatchSampleRate();
    void logAudioConfig (const juce::String& contextLabel) const;

    /** Reports it when the device actually open is not the one that was asked
        for. See Source/DevicePolicy.hpp for why this cannot be left to JUCE.

        Must run BEFORE the audio device state is written back, because the
        comparison is against the stored request and saving overwrites it.
    */
    void reportDeviceSubstitutionIfAny (const juce::String& contextLabel);

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

    /** Detaches this listener from every processor currently in the graph.

        Called before graph.clear(), so a chain reload is not holding a listener
        registration on processors that are being destroyed.
    */
    void stopListeningToAll();

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
    juce::PopupMenu menu;
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;

    // Wraps the player so the device input and output can be metered. Declared
    // after player because it holds a reference to it, and member construction
    // follows declaration order.
    lighthost::metering::DeviceTap deviceTap { player };
    NodeID inputNodeId;
    NodeID outputNodeId;

    // Null IS the invalidation, so there is no separate dirty flag to keep in
    // step with it. Handed out by value, so a snapshot already given to a caller
    // stays alive and intact after this is replaced.
    mutable ChainSnapshot sortedPluginCache;

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
        juce::MemoryBlock       savedState;
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
                                const juce::MemoryBlock& savedState);
    void restorePluginState (juce::AudioProcessorGraph::Node& node,
                             juce::AudioProcessorGraph::NodeID nodeId,
                             const juce::MemoryBlock& savedState);

    /** Where this instance keeps its per-plugin state files.

        Returned by value rather than held: a Vault is a File and nothing else,
        so there is no lifetime question and no state to go stale. Keyed off the
        settings filename, so a -multi-instance run does not share a state
        directory with the primary configuration.
    */
    [[nodiscard]] lighthost::state::Vault stateVault() const;

    /** One-shot move of pre-5.0.0 base64 state out of the settings document.
        Writes the file first and only then drops the key.
    */
    void migrateStateToVault();

    // Fingerprint of the bytes last written for each identity, so a save that
    // changed nothing does not gzip and rewrite megabytes. In-memory only: a
    // fresh process writes once per plugin and then settles.
    std::map<juce::String, juce::uint64> lastWrittenState;
    void onAllPluginsLoaded (int generation);

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;

    std::unique_ptr<PreferencesWindow> preferencesWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconMenu)
};
