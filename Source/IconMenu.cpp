#include "IconMenu.hpp"
#include "ConfirmPolicy.hpp"
#include "DevicePolicy.hpp"
#include "Lanes.hpp"
#include "NodeIds.hpp"
#include "GraphTopology.hpp"
#include "PluginChainStore.hpp"
#include "PluginState.hpp"
#include "UiMetrics.hpp"
#include "SampleRatePolicy.hpp"
#include "PluginWindow.h"
#include "PreferencesWindow.h"
#include <BinaryData.h>
#include <algorithm>
#include <exception>

#if JUCE_WINDOWS
#include <windows.h>
#include <shlobj.h>
#endif

using namespace juce;

namespace metrics = lighthost::ui::metrics;

// Owns the per-plugin settings keys. Made where it is used rather than held as
// state: it is a reference to the settings plus a staging area, and a long-lived
// one would carry a half-finished edit into the next operation.
using ChainStore = lighthost::chain::Store;

//==============================================================================
class IconMenu::PluginListWindow final : public DocumentWindow
{
public:
    PluginListWindow (IconMenu& owner_, AudioPluginFormatManager& pluginFormatManager)
        : DocumentWindow ("Available Plugins",
                          LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                          DocumentWindow::minimiseButton | DocumentWindow::closeButton),
          owner (owner_)
    {
        const File deadMansPedalFile (getAppProperties().getUserSettings()
            ->getFile().getSiblingFile ("RecentlyCrashedPluginsList"));

        setContentOwned (new AugmentedPluginListComponent (
                             pluginFormatManager,
                             owner.knownPluginList,
                             deadMansPedalFile,
                             getAppProperties().getUserSettings(),
                             [&ownerRef = owner] (const String& message)
                             {
                                 ownerRef.reportStatus (message);
                             }),
                         true);

        setUsingNativeTitleBar (true);
        setResizable (true, false);

        // The maximum was a hard-coded 800x1500 inherited from 2016, which capped
        // the window far below any modern display and truncated the Description
        // column. Derive it from the whole desktop so a wide window is possible on
        // a multi-monitor setup and the cap is never what stops a resize. The
        // minimums are unchanged: they are what the table needs to stay usable.
        const auto desktop = Desktop::getInstance().getDisplays().getTotalBounds (true);
        setResizeLimits (300, 400,
                         jmax (300, desktop.getWidth()),
                         jmax (400, desktop.getHeight()));
        setTopLeftPosition (60, 60);

        restoreWindowStateFromString (getAppProperties().getUserSettings()->getValue ("listWindowPos"));
        setVisible (true);
    }

    ~PluginListWindow() override
    {
        getAppProperties().getUserSettings()->setValue ("listWindowPos", getWindowStateAsString());
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        // Closing this window used to purge every scanned plugin with fewer than
        // two input or output channels from the user's plugin list, permanently.
        // See the note on removePluginsLackingInputOutput's removal in the 5.0.0
        // changelog: the scan results are the user's, not ours to prune.

        #if JUCE_MAC
        Process::setDockIconVisible (false);
        #endif

        owner.pluginListWindow.reset();
    }

private:
    //==========================================================================
    // PluginListComponent subclass that injects "Scan Custom Folder..." into
    // the existing Options popup menu, keeping the standard items intact.
    class AugmentedPluginListComponent final : public PluginListComponent
    {
    public:
        AugmentedPluginListComponent (AudioPluginFormatManager& fmgr,
                                      KnownPluginList& kpl,
                                      const File& deadMansFile,
                                      PropertiesFile* propsFile,
                                      std::function<void (const String&)> reporter)
            : PluginListComponent (fmgr, kpl, deadMansFile, propsFile),
              formatManager (fmgr),
              knownList     (kpl),
              deadMansPedal (deadMansFile),
              properties    (propsFile),
              report        (std::move (reporter))
        {
            // Replace the built-in Options button onClick so we can append our item.
            getOptionsButton().onClick = [this]
            {
                // Not "menu": IconMenu has a field of that name, and a local
                // shadowing it is the kind of thing that reads correctly right
                // up until someone edits the lambda and means the other one.
                auto optionsMenu = createOptionsMenu();
                optionsMenu.addSeparator();
                optionsMenu.addItem (kScanCustomFolderID, "Scan Custom Folder...");
                optionsMenu.showMenuAsync (
                    PopupMenu::Options()
                        .withDeletionCheck (*this)
                        .withTargetComponent (getOptionsButton()),
                    [this] (int result)
                    {
                        if (result == kScanCustomFolderID)
                            browseAndScan();
                    });
            };

            // juce::PluginListComponent hard-codes setRowHeight(20) and
            // setHeaderHeight(22) in its own constructor. Those two metrics are
            // most of why this table reads as cramped, and a look and feel is
            // never consulted about them, so they are raised here, where the
            // component is actually made.
            getTableListBox().setRowHeight (26);
            getTableListBox().setHeaderHeight (28);
        }

        void resized() override
        {
            PluginListComponent::resized();

            // JUCE gives the Options button a 24px strip and then calls
            // changeWidthToFitText on it, which shrinks it to the width of its
            // own label. That leaves the smallest control in the application
            // sitting in the corner as an awkward click target. Lay it out at a
            // deliberate size instead, and hand the table back the difference.
            //
            // The base call above still runs, so any child JUCE adds to this
            // component in a later version is still positioned by JUCE; only the
            // two children it lays out today are overridden here.
            auto bounds = getLocalBounds().reduced (kListMargin);
            auto strip  = bounds.removeFromBottom (metrics::pushButtonStripHeight());

            getOptionsButton().setBounds (
                metrics::pushButton (strip.removeFromLeft (kOptionsWidth), kOptionsWidth));

            bounds.removeFromBottom (kOptionsGap);
            getTableListBox().setBounds (bounds);
        }

    private:
        static constexpr int kScanCustomFolderID = 9999;

        // kListMargin matches PluginListComponent::resized, so the table keeps
        // the inset it already had; only the strip below it belongs to us.
        static constexpr int kListMargin    = 2;
        static constexpr int kOptionsWidth  = 110;
        static constexpr int kOptionsGap    = 6;

        AudioPluginFormatManager&    formatManager;
        KnownPluginList&             knownList;
        File                         deadMansPedal;
        PropertiesFile*              properties = nullptr;
        std::function<void (const String&)> report;
        std::unique_ptr<FileChooser> chooser;

        //----------------------------------------------------------------------
        class ScanJob final : public ThreadWithProgressWindow
        {
        public:
            ScanJob (AudioPluginFormatManager& fmgr,
                     KnownPluginList& kpl,
                     const File& deadMans,
                     const File& dir)
                : ThreadWithProgressWindow ("Scanning plugins...", true, true),
                  formatManager (fmgr),
                  knownList     (kpl),
                  deadMansPedal (deadMans),
                  scanDir       (dir) {}

            void run() override
            {
                for (int i = 0; i < formatManager.getNumFormats(); ++i)
                {
                    auto* fmt = formatManager.getFormat (i);
                    if (fmt == nullptr || ! fmt->canScanForPlugins()) continue;

                    FileSearchPath path;
                    path.add (scanDir);

                    // true = recursive, matching what JUCE hard-codes for its own
                    // scan. A folder holding plugins in per-vendor subdirectories
                    // is therefore found from the parent, and the stored path does
                    // not have to name each subdirectory.
                    PluginDirectoryScanner scanner (knownList, *fmt, path, true, deadMansPedal);
                    String name;
                    while (! threadShouldExit() && scanner.scanNextFile (true, name))
                        setStatusMessage ("Scanning: " + name);

                    // Kept, not discarded. These are files that looked like
                    // plugins and would not load. JUCE surfaces them after its own
                    // scan; a custom-folder scan used to drop them, so a folder
                    // where every plugin failed presented exactly like a folder
                    // that was scanned cleanly.
                    failed.addArray (scanner.getFailedFiles());
                }
            }

            [[nodiscard]] const StringArray& getFailedFiles() const noexcept { return failed; }

        private:
            AudioPluginFormatManager& formatManager;
            KnownPluginList&          knownList;
            File                      deadMansPedal;
            File                      scanDir;
            StringArray               failed;
        };

        //----------------------------------------------------------------------
        void browseAndScan()
        {
            chooser = std::make_unique<FileChooser> (
                "Select Plugin Folder",
                File::getSpecialLocation (File::globalApplicationsDirectory));

            chooser->launchAsync (
                FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                [this] (const FileChooser& fc)
                {
                    const auto dir = fc.getResult();
                    if (! dir.isDirectory()) return;

                    ScanJob job (formatManager, knownList, deadMansPedal, dir);

                    if (! job.runThread())
                        return;                       // cancelled from the dialog

                    rememberSearchPath (dir);
                    reportScanOutcome (dir, job.getFailedFiles());
                });
        }

        /** Adds a hand-picked folder to the stored search path of every format
            that can scan.

            Without this the folder was scanned once and then forgotten: the
            standard "Scan for new or updated plug-ins" item never looked there
            again, so an updated plugin in a custom folder silently kept the
            description recorded the first time.
        */
        void rememberSearchPath (const File& dir)
        {
            if (properties == nullptr)
                return;

            for (int i = 0; i < formatManager.getNumFormats(); ++i)
            {
                auto* fmt = formatManager.getFormat (i);

                if (fmt == nullptr || ! fmt->canScanForPlugins())
                    continue;

                auto path = getLastSearchPath (*properties, *fmt);

                // Scanning is recursive, so a parent already in the path covers
                // this folder and adding it would only make the list longer.
                if (path.isFileInPath (dir, true))
                    continue;

                path.addIfNotAlreadyThere (dir);
                setLastSearchPath (*properties, *fmt, path);
            }
        }

        /** Reports a scan that did not go cleanly. A scan that worked says
            nothing: the table itself is the result.

            Deliberately says nothing about how many plugins were found. It used
            to report "No plugins found" whenever the known-plugin count had not
            grown, and that count does not grow on a successful rescan either --
            scanNextFile skips what is already listed, and an updated plugin is
            replaced in place rather than added. So the one case this was meant
            to help, re-scanning a folder after updating a plugin in it, was told
            that nothing was there.
        */
        void reportScanOutcome (const File& dir, const StringArray& failed)
        {
            if (! report || failed.isEmpty())
                return;

            StringArray names;

            for (const auto& f : failed)
                names.add (File (f).getFileName());

            report (String (failed.size())
                    + (failed.size() == 1 ? " plugin in " : " plugins in ")
                    + dir.getFileName() + " could not be loaded: "
                    + names.joinIntoString (", "));
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AugmentedPluginListComponent)
    };

    //==========================================================================

    IconMenu& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginListWindow)
};

//==============================================================================
IconMenu::IconMenu()
{
    juce::Logger::writeToLog ("IconMenu: constructing");

    // JUCE 8: addDefaultFormats() removed from AudioPluginFormatManager.
    // Use the free function addDefaultFormatsToManager() instead.
    addDefaultFormatsToManager (formatManager);

    // Any problem reported from here on shows up in the tray tooltip, and in the
    // Preferences window when it happens to be open.
    status.onChange = [this]
    {
        refreshTooltip();

        if (preferencesWindow != nullptr)
            preferencesWindow->setStatusMessage (status.mostRecent());
    };

    // Audio device initialization. initialise returns a description of why it
    // could not open a device, which was previously discarded: the app then looked
    // like it was running while passing no audio at all.
    {
        const auto savedAudioState = getAppProperties().getUserSettings()->getXmlValue (lighthost::keys::audioDeviceState);
        const auto error = deviceManager.initialise (2, 2, savedAudioState.get(), true);

        if (error.isNotEmpty())
            status.report ("Audio device could not be opened: " + error);

        // Separate from the error above, which cannot catch this: JUCE replaces
        // that string with the fallback's own result, and the fallback succeeds.
        reportDeviceSubstitutionIfAny ("startup");
    }

    player.setProcessor (&graph);
    // The tap is registered, not the player, so it can see the output buffer.
    // AudioDeviceManager gives only its FIRST callback the real output buffer and
    // hands every later one a temporary it sums, so a second callback registered
    // alongside the player would measure silence.
    deviceManager.addAudioCallback (&deviceTap);
    deviceManager.addChangeListener (this);

    logAudioConfig ("startup");

    // Known plugins
    if (auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue (lighthost::keys::pluginList))
        knownPluginList.recreateFromXml (*savedPluginList);

    knownPluginList.addChangeListener (this);

    // Active plugins
    if (auto savedPluginListActive = getAppProperties().getUserSettings()->getXmlValue (lighthost::keys::pluginListActive))
        activePluginList.recreateFromXml (*savedPluginListActive);

    // Settings written by 4.0.3 and earlier are keyed on the plugin's name and
    // version, so a plugin update orphaned everything the user had set. Move them
    // onto the stable keys once, now that the chain is known.
    {
        auto* settings = getAppProperties().getUserSettings();
        ChainStore store (*settings);

        const auto types = activePluginList.getTypes();
        const std::vector<PluginDescription> chain (types.begin(), types.end());

        if (const auto migrated = store.migrateIfNeeded (chain); migrated > 0)
            juce::Logger::writeToLog ("IconMenu: migrated chain settings for "
                                      + juce::String (migrated) + " plugin(s)");

        flushSettings (*settings, "chain settings migration");
    }

    migrateStateToVault();

    loadActivePlugins();
    activePluginList.addChangeListener (this);

    setIcon();
    refreshTooltip();
}

IconMenu::~IconMenu()
{
    juce::Logger::writeToLog ("IconMenu: shutting down");

    // Cut the status sink's callback before anything below can report.
    //
    // The callback dereferences preferencesWindow, and ~IconMenu is the one path
    // on which that pointer can be non-null while the window is being destroyed.
    // The close path is safe and looks like the hazard: preferencesWindow.reset()
    // assigns null BEFORE deleting, so the callback sees nullptr. ~unique_ptr does
    // not, and this destructor never resets the member -- so the window is still
    // reachable through it for the whole of its own teardown.
    //
    // Reachable, not theoretical: quit from the tray with Preferences open, a
    // lane trim moved but not written, and a settings file that cannot be
    // written. ~PreferencesContentComponent calls commitLaneTrim ->
    // persistLaneGainDb -> flushSettings, the write fails, status.report fires
    // this callback, and setStatusMessage calls resized() on a panel that is
    // half destroyed.
    //
    // Clearing it costs the last few shutdown reports their UI, which is the
    // right trade: they still reach the log, and there is no window left to
    // read them in.
    status.onChange = nullptr;

    // Detach the audio callback FIRST — deviceManager outlives player and graph
    // in member-destruction order (declared before them, destroyed after them).
    // Without this, deviceManager's real-time thread keeps firing
    // audioDeviceIOCallback() into player/graph while they are being destroyed,
    // which is a guaranteed use-after-free on every shutdown path (visibly
    // crashes on system restart/shutdown where the OS disrupts audio in parallel).
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&deviceTap);
    player.setProcessor (nullptr);

    // A plugin may have reported a latency change moments ago. Nothing should
    // rewire the graph while it is being torn down.
    cancelPendingUpdate();

    // And stop listening, for the same reason loadActivePlugins does these two
    // lines together.
    //
    // The cancel alone is not enough, and ~AsyncUpdater is too late to help:
    // IconMenu derives from AsyncUpdater, so that destructor runs after this
    // whole body. closeAllCurrentlyOpenWindows() at the end of this function
    // deletes every editor and then PUMPS THE MESSAGE QUEUE, and plugins
    // commonly report a latency change as their editor closes. Still registered
    // as a listener, we would take that report, arm the updater, and have it
    // delivered by that same pump -- running reconnectGraph() mid-teardown,
    // which adds probe nodes to a graph about to be destroyed and pushes a
    // bypass parameter into a plugin whose editor has just gone, and
    // refreshPreferencesIfOpen(), which drives a window this destructor
    // deliberately never resets.
    //
    // PluginWindow.cpp states this invariant as the caller's responsibility and
    // names loadActivePlugins as the example that honours it. This path did not.
    stopListeningToAll();

    // Abandon any in-flight load before saving. Bumping the generation makes the
    // pending createPluginInstanceAsync callbacks no-op when they arrive. Their
    // captured SafePointer already guards against this object being gone; the
    // generation bump additionally stops a late callback mutating the graph while
    // we tear down. There is no worker thread to stop, so this cannot block.
    ++pluginLoadGeneration;
    pendingLoads.clear();

    savePluginStates();

    // Close any editor still on screen, AFTER the states above have been read
    // out of the live instances and BEFORE the graph goes.
    //
    // This was called from exactly one place, loadActivePlugins, so quitting
    // from the tray with a plugin's Settings window open leaked both. A
    // PluginWindow holds a refcounted Node::Ptr, so the node and the hosted
    // plugin outlived the graph that owned them, and the registry of open
    // windows is a function-local static Array that is destroyed at exit
    // without deleting what it points at. The plugin's own editor teardown
    // therefore never ran -- third-party code, skipped on every shutdown.
    PluginWindow::closeAllCurrentlyOpenWindows();
}

//==============================================================================
void IconMenu::cancelPluginLoading()
{
    if (pendingLoads.empty())
        return;

    // Bump the generation so the in-flight async callback drops its result, and
    // drop the queue so no further plugins are dispatched.
    ++pluginLoadGeneration;
    pendingLoads.clear();
    nextLoadIndex = 0;
    refreshTooltip();
    juce::Logger::writeToLog ("IconMenu: cancelled in-flight plugin load");
}

//==============================================================================
void IconMenu::setIcon()
{
    // setIconImage takes (colourImage, templateImage). The second is used only on
    // macOS, where the menu bar wants a monochrome template the OS can invert for
    // light and dark appearance. Versions up to 4.0.3 passed the colour art for
    // both, which put a full-size colour app icon in the macOS menu bar.
    const auto colourIcon = ImageFileFormat::loadFrom (BinaryData::TrayIcon_png,
                                                       BinaryData::TrayIcon_pngSize);
    const auto templateIcon = ImageFileFormat::loadFrom (BinaryData::TrayIconTemplate_png,
                                                         BinaryData::TrayIconTemplate_pngSize);

    setIconImage (colourIcon, templateIcon);
}

//==============================================================================
void IconMenu::loadActivePlugins()
{
    constexpr auto kInputNodeId  = lighthost::nodeids::input;
    constexpr auto kOutputNodeId = lighthost::nodeids::output;

    juce::Logger::writeToLog ("IconMenu: loadActivePlugins begin");

    // Cancel any in-flight load. Bumping the generation makes queued async
    // callbacks from the previous run drop their results when they arrive.
    ++pluginLoadGeneration;
    pendingLoads.clear();
    nextLoadIndex = 0;
    statesNotRestored.clear();   // fresh graph — no failed restores to remember yet

    // Stop anything that could rewire the graph while it is being replaced.
    //
    // The generation bump above defuses a late createPluginInstanceAsync
    // callback, but not the AsyncUpdater: audioProcessorChanged fires it
    // whenever a hosted plugin reports a latency change, and plugins commonly
    // report one as their editor closes -- which is precisely what the next line
    // does. ~IconMenu has always cancelled it for the same reason; this path
    // never did, leaving one re-entrancy channel the generation counter does not
    // cover.
    cancelPendingUpdate();

    // And stop listening before the processors are destroyed, rather than
    // relying on ~AudioProcessor to unregister us.
    stopListeningToAll();

    sortedPluginCache.reset();
    PluginWindow::closeAllCurrentlyOpenWindows();

    // UpdateKind::none here and on every mutation down to reconnectGraph(), so
    // the whole reload reaches the audio thread as ONE render sequence. The
    // defaults published one for the clear, one per IO node and one per lane
    // trim -- every one of them a chain in the middle of being assembled, and
    // the first of them silence. The old sequence keeps running and keeps
    // passing audio until the rebuild at the end of reconnectGraph() replaces
    // it, because it holds a Node::Ptr to each node it renders
    // (juce_AudioProcessorGraph.cpp, NodeOp) and so keeps the outgoing
    // instances alive.
    //
    // Nothing between here and that reconnectGraph() may return early: the
    // graph would be left mutated and unpublished, running the old chain
    // forever.
    graph.clear (AudioProcessorGraph::UpdateKind::none);

    // Checked, like createLaneGainNodes and syncProbeNodes already are. Without
    // an IO node reconnectGraph simply skips the wiring it cannot do, so the
    // failure presents as a host that is running and passing no audio at all.
    const auto inputNode = graph.addNode (
        std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (
            AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode), kInputNodeId,
        AudioProcessorGraph::UpdateKind::none);

    const auto outputNode = graph.addNode (
        std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (
            AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode), kOutputNodeId,
        AudioProcessorGraph::UpdateKind::none);

    if (inputNode == nullptr || outputNode == nullptr)
        status.report ("The audio graph could not be built, so no audio is being"
                       " passed. Restarting Light Host is the only fix.");

    inputNodeId  = kInputNodeId;
    outputNodeId = kOutputNodeId;

    // One trim per lane, on reserved ids, created once here rather than allocated
    // like plugin nodes: there are exactly kNumLanes of them and they outlive every
    // chain edit. A lane whose trim could not be added is wired straight to the
    // output instead, so a failure here costs the trim and not the audio.
    createLaneGainNodes();

    // Wire the passthrough immediately so audio flows while plugins load. It
    // runs through lane 0's trim, not at unity: createLaneGainNodes above has
    // already seeded every trim from settings, so a lane muted before the last
    // quit is muted for this window too. Ordering matters -- the trims have to
    // exist before this call, or the layout has no id to route through and the
    // window runs at full level.
    reconnectGraph();

    const auto sortedSnapshot = getTimeSortedList();
    const auto& sorted = *sortedSnapshot;

    if (sorted.empty())
        return;

    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);

    pendingLoads.reserve (sorted.size());

    const auto vault = stateVault();

    for (const auto& plugin : sorted)
    {
        auto nodeIdVal = store.readNodeId (plugin);

        if (nodeIdVal == 0)
        {
            nodeIdVal = store.allocateNodeId();

            if (nodeIdVal == 0)
            {
                // The id range is exhausted. Skipping the plugin and saying so
                // is the only honest option: the alternative is handing the
                // graph a reserved id, which addNode refuses with nothing but a
                // debug assertion, after which getNodeForId returns the graph's
                // audio INPUT node for this plugin.
                status.report ("Ran out of plugin node ids, so " + plugin.name
                               + " was not loaded. Deleting and re-adding the"
                                 " chain resets them.");
                continue;
            }

            store.stageNodeId (plugin, nodeIdVal);
        }

        auto vaultOutcome = lighthost::state::Vault::ReadResult::absent;
        auto savedState = vault.read (ChainStore::identityOf (plugin), &vaultOutcome);

        if (vaultOutcome == lighthost::state::Vault::ReadResult::corrupt)
        {
            // Present and unusable, which is NOT the same as absent. The file is
            // the only copy of that preset, so the node is recorded as
            // un-restored -- savePluginStates skips those -- and the legacy
            // fallback below is deliberately not tried: it would find nothing,
            // report "nothing saved", and license the next save to overwrite the
            // corrupt file with factory defaults. That is the same shape as the
            // base64 preset loss fixed in 5.2.0.
            statesNotRestored.insert (static_cast<uint32> (nodeIdVal));

            status.report (plugin.name + " has a damaged saved preset, so it has"
                                         " loaded its defaults. The file has been"
                                         " left alone.");
        }
        else if (savedState.getSize() == 0)
        {
            // Falls back to the pre-5.0.0 key, so a migration that could not
            // write its file presents as a plugin that still has its preset
            // rather than one that lost it.
            if (lighthost::state::decodeLegacyState (store.readState (plugin), savedState)
                    == lighthost::state::DecodeResult::failed)
            {
                // Something is stored and will not decode. It is the only copy,
                // so record the node as un-restored: savePluginStates skips
                // those, and without this the plugin loaded at factory defaults
                // and then wrote them over the user's preset on the next save.
                statesNotRestored.insert (static_cast<uint32> (nodeIdVal));

                juce::Logger::writeToLog ("IconMenu: " + plugin.name
                                          + " has legacy state that will not decode;"
                                            " loading defaults and preserving the blob");
            }
        }

        pendingLoads.push_back ({ plugin,
                                  std::move (savedState),
                                  static_cast<uint32> (nodeIdVal) });
    }

    store.commit();
    flushSettings (*settings, "assigning plugin node ids");

    refreshTooltip();
    loadNextPlugin();
}

//==============================================================================
// Loads the next queued plugin on the message thread. createPluginInstanceAsync
// does the DLL load and factory call on this thread and calls back here when the
// instance is ready, at which point we chain to the next one. This keeps the
// message loop pumping between plugins so the UI stays responsive.
void IconMenu::loadNextPlugin()
{
    if (nextLoadIndex >= pendingLoads.size())
    {
        onAllPluginsLoaded (pluginLoadGeneration);
        return;
    }

    auto& spec = pendingLoads[nextLoadIndex];

    const auto sr = graph.getSampleRate() > 0.0 ? graph.getSampleRate() : 44100.0;
    const auto bs = graph.getBlockSize()  > 0   ? graph.getBlockSize()  : 512;

    const auto generation = pluginLoadGeneration;
    const auto nodeId     = spec.nodeId;
    const auto name       = spec.description.name;

    Component::SafePointer<IconMenu> safe (this);

    // The state is MOVED into the lambda, not copied. It used to be copied into
    // a local and then copied again by the capture, on top of the copy
    // pendingLoads already held -- three copies of a blob that PluginStateVault
    // measured at 4,126,524 bytes for one real plugin, so tens of megabytes of
    // memcpy on the message thread during startup, in the subsystem whose whole
    // reason for existing is that these are too large to move around casually.
    //
    // Safe because this entry is finished with the moment it is dispatched:
    // nextLoadIndex advances and nothing reads spec.savedState again.
    formatManager.createPluginInstanceAsync (
        spec.description, sr, bs,
        [safe, generation, nodeId, name, savedState = std::move (spec.savedState)]
            (std::unique_ptr<AudioPluginInstance> instance, const juce::String& error) mutable
        {
            auto* im = safe.getComponent();
            if (im == nullptr)
                return;   // IconMenu gone; instance cleaned up by unique_ptr

            if (instance == nullptr)
                im->status.report ("Plugin load failed: " + name + " - " + error);

            im->onPluginInstanceReady (std::move (instance),
                                       AudioProcessorGraph::NodeID { nodeId },
                                       generation,
                                       savedState);
        });
}

//==============================================================================
void IconMenu::onPluginInstanceReady (std::unique_ptr<AudioProcessor> instance,
                                      AudioProcessorGraph::NodeID nodeId,
                                      int generation,
                                      const juce::MemoryBlock& savedState)
{
    if (generation != pluginLoadGeneration)
        return;  // stale result — a newer load supersedes this one and owns the chain

    // Add the node first, then restore into the live node, so that a failed
    // restore can be recorded against a node that actually exists.
    if (instance != nullptr)
    {
        // Guard against the plugin having been deleted from the active list while
        // it was loading. If its NodeID is no longer in settings it is unwanted;
        // the unique_ptr cleans it up.
        const ChainStore store (*getAppProperties().getUserSettings());
        bool stillWanted = false;

        for (const auto& plugin : activePluginList.getTypes())
        {
            if (static_cast<uint32> (store.readNodeId (plugin)) == nodeId.uid)
            {
                stillWanted = true;
                break;
            }
        }

        if (stillWanted)
        {
            // UpdateKind::none, like every other graph mutation in this file.
            // The default is sync, which rebuilds and publishes a render
            // sequence per plugin -- so an eight-plugin chain published ten
            // where two would do, each an O(nodes^2) ordering pass plus three
            // block-sized buffer allocations. Nothing between here and the
            // rebuild in onAllPluginsLoaded reads the sequence.
            //
            // It also moves prepareToPlay to that final rebuild, i.e. AFTER
            // restorePluginState below, so a plugin sizes its buffers knowing
            // its real settings. OfflineRender already prepares in that order.
            if (auto node = graph.addNode (std::move (instance), nodeId,
                                           AudioProcessorGraph::UpdateKind::none))
            {
                restorePluginState (*node, nodeId, savedState);

                if (auto* processor = node->getProcessor())
                    listenTo (*processor);

                juce::Logger::writeToLog ("IconMenu: plugin instance ready for node "
                                          + juce::String ((int) nodeId.uid));
            }
            else
            {
                // addNode refused the ID (already present). Do not claim success:
                // silently dropping the instance here is how a plugin ends up in
                // the UI with no audio and no error.
                juce::Logger::writeToLog ("IconMenu: addNode rejected node "
                                          + juce::String ((int) nodeId.uid)
                                          + " (duplicate id); instance discarded");
            }
        }
    }

    // Chain to the next queued plugin.
    ++nextLoadIndex;
    loadNextPlugin();
}

// Restores saved state into a live graph node. On failure the node keeps its
// factory defaults and its id is recorded in statesNotRestored, so
// savePluginStates() will not overwrite the good blob. The restore itself lives
// in PluginState.hpp so its failure handling is unit tested.
void IconMenu::restorePluginState (AudioProcessorGraph::Node& node,
                                   AudioProcessorGraph::NodeID nodeId,
                                   const juce::MemoryBlock& savedState)
{
    auto* processor = node.getProcessor();
    if (processor == nullptr)
        return;

    switch (lighthost::state::restoreInto (*processor, savedState))
    {
        case lighthost::state::RestoreResult::restored:
        case lighthost::state::RestoreResult::nothingSaved:
            break;

        case lighthost::state::RestoreResult::failed:
            statesNotRestored.insert (nodeId.uid);
            status.report ("A plugin refused to restore its saved settings. Its stored "
                           "preset has been left alone rather than overwritten.");
            break;
    }
}

//==============================================================================
// Lane trims. Reserved ids well clear of the plugin ids, which are allocated from
// 1 upwards, and of the two IO nodes.
juce::AudioProcessorGraph::NodeID IconMenu::laneGainNodeId (int lane)
{
    return lighthost::nodeids::laneGain (lane);
}

void IconMenu::createLaneGainNodes()
{
    const ChainStore store (*getAppProperties().getUserSettings());

    for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
    {
        // Trimmed BEFORE it is handed to the graph, not after. addNode prepares
        // the node, and gain::Processor::prepareToPlay seeds the ramp from
        // whatever gain is set at that instant -- so adding first and trimming
        // after left the ramp seeded at 0 dB and made the first block sweep
        // from unity DOWN to the stored trim, which is the fade that
        // prepareToPlay's own comment says it exists to prevent. Reachable
        // whenever the chain is built in one call stack, i.e. every startup and
        // every Apply that adds a plugin.
        auto trim = std::make_unique<lighthost::gain::Processor>();
        trim->setGainDb (store.readLaneGainDb (lane));

        // UpdateKind::none for the reason given at the top of
        // loadActivePlugins: the caller rewires and rebuilds once, so a publish
        // per lane only puts half-assembled chains in front of the audio
        // thread.
        const auto node = graph.addNode (std::move (trim), laneGainNodeId (lane),
                                         juce::AudioProcessorGraph::UpdateKind::none);

        if (node == nullptr)
            status.report ("Lane " + juce::String (lane)
                           + " trim could not be created; that lane runs at unity.");
    }
}

// Metering probes. Reserved ids above the lane trims, and created only while the
// signal view is open: a diagnostic that costs something all day to be useful for
// a minute is not a trade this application makes.
juce::AudioProcessorGraph::NodeID IconMenu::probeNodeId (int index)
{
    return lighthost::nodeids::probe (index);
}

std::vector<int> IconMenu::syncProbeNodes()
{
    // Re-derived from the chain every time rather than tracked, so adding or
    // removing a plugin while the panel is open cannot leave a stale probe
    // behind or a new plugin unprobed.
    const auto wanted = signalViewEnabled
                            ? juce::jmin (static_cast<int> (getTimeSortedList()->size()), kMaxProbes)
                            : 0;

    std::vector<int> retired;

    for (int i = 0; i < kMaxProbes; ++i)
    {
        const auto id = probeNodeId (i);
        const bool shouldExist = i < wanted;
        const bool doesExist   = graph.getNodeForId (id) != nullptr;

        if (shouldExist && ! doesExist)
        {
            const auto slot = static_cast<size_t> (i);

            // UpdateKind::none, like the removal below, and this line used to
            // default to sync while the comment beside the removal claimed
            // "every other mutation here uses it". Opening the signal view on a
            // chain of six therefore published six render sequences before the
            // one reconnectGraph builds, each of them a graph with a probe
            // spliced in and nothing wired to it.
            if (graph.addNode (std::make_unique<lighthost::metering::Probe> (probeMeters[slot]),
                               id, juce::AudioProcessorGraph::UpdateKind::none) == nullptr)
            {
                juce::Logger::writeToLog ("IconMenu: probe " + juce::String (i)
                                          + " could not be created; that position shows no level");
            }
        }
        else if (doesExist && ! shouldExist)
        {
            // UpdateKind::none: one rebuild at the end of reconnectGraph, not
            // one per node, so the audio thread never sees a half-wired graph.
            graph.removeNode (id, juce::AudioProcessorGraph::UpdateKind::none);

            // The meter is NOT cleared here. Because the removal above does not
            // republish, the live render sequence still contains this probe and
            // the audio thread goes on calling Probe::processBlock into its
            // meter until reconnectGraph's rebuild swaps that sequence out.
            // Meter::publish is a load-modify-store on heldPeak, so a block
            // landing between the reset and the rebuild put back exactly the
            // level the reset existed to clear, and the row kept showing a
            // level for a probe that no longer exists. Returned to the caller
            // to be cleared after the rebuild instead.
            retired.push_back (i);
        }
    }

    return retired;
}

void IconMenu::setSignalViewEnabled (bool shouldBeEnabled)
{
    if (signalViewEnabled == shouldBeEnabled)
        return;

    signalViewEnabled = shouldBeEnabled;

    juce::Logger::writeToLog (juce::String ("IconMenu: signal view ")
                              + (shouldBeEnabled ? "opened; inserting probes"
                                                 : "closed; removing probes"));

    // reconnectGraph syncs the probe nodes and rewires in one pass, so the
    // change reaches the audio thread as a single render sequence.
    reconnectGraph();
}

lighthost::metering::Meter* IconMenu::getProbeMeter (int index)
{
    // Always a valid pointer for a valid slot, whether or not a probe currently
    // exists. The meter outlives the graph on purpose: the UI caches what this
    // returns, and a pointer into a Probe would dangle the moment the chain
    // reloaded. With no probe feeding it, the meter simply reads silence.
    if (! juce::isPositiveAndBelow (index, lighthost::nodeids::maxProbes))
        return nullptr;

    return &probeMeters[static_cast<size_t> (index)];
}

std::vector<juce::String> IconMenu::getCommittedChainNames()
{
    // getTimeSortedList is the same source reconnectGraph walks when it decides
    // which chain position carries which probe, so index i here and probe i
    // there are the same plugin by construction rather than by two paths
    // happening to agree.
    const auto sorted = getTimeSortedList();

    // Capped at the same limit getProbeMeter enforces, so the two stay the same
    // length. refreshSignalView pairs them one to one; uncapped, chain position
    // 33 and up got a labelled row whose meter is null, which the update loop
    // skips and the painter therefore draws at kFloorDb for ever. A permanent
    // silent row is a worse answer than no row: IconMenu.hpp says a position
    // past the cap "just stops being probed", and a row reading silence says
    // the plugin is passing nothing.
    const auto limit = juce::jmin (static_cast<size_t> (sorted->size()),
                                   static_cast<size_t> (lighthost::nodeids::maxProbes));

    std::vector<juce::String> names;
    names.reserve (limit);

    for (size_t i = 0; i < limit; ++i)
        names.push_back ((*sorted)[i].name);

    return names;
}

lighthost::gain::Processor* IconMenu::laneGainProcessor (int lane)
{
    if (auto* node = graph.getNodeForId (laneGainNodeId (lane)))
        return dynamic_cast<lighthost::gain::Processor*> (node->getProcessor());

    return nullptr;
}

float IconMenu::getLaneGainDb (int lane) const
{
    const ChainStore store (*getAppProperties().getUserSettings());
    return store.readLaneGainDb (lane);
}

void IconMenu::setLaneGainDb (int lane, float decibels)
{
    // Applied to the running graph immediately: this is a level control, and a
    // fader you cannot hear while dragging is not a fader. The trim ramps
    // internally, so no rewire and no rebuild is needed.
    if (auto* processor = laneGainProcessor (lane))
        processor->setGainDb (decibels);
}

void IconMenu::persistLaneGainDb (int lane, float decibels)
{
    // Called when the drag ends rather than on every value change: each write
    // rewrites the whole settings document, so persisting per pixel would be
    // hundreds of rewrites for one gesture.
    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);

    store.stageLaneGainDb (lane, decibels);
    store.commit();
    flushSettings (*settings, "saving a lane trim");
}

//==============================================================================
void IconMenu::flushSettings (juce::PropertiesFile& settings, const juce::String& context)
{
    // saveIfNeeded returns false when the file could not be written: a full disk,
    // a permission problem, a backup agent holding the file open. Every one of
    // these call sites used to ignore it, so the user lost their edits silently.
    if (! settings.saveIfNeeded())
        status.report ("Settings could not be saved (" + context
                       + "). Recent changes may be lost when Light Host closes.");
}

lighthost::state::Vault IconMenu::stateVault() const
{
    const auto settingsFile = getAppProperties().getUserSettings()->getFile();

    return lighthost::state::Vault::beside (settingsFile);
}

bool IconMenu::forgetPluginState (const lighthost::state::Vault& vault,
                                  const juce::String& identity)
{
    if (! vault.erase (identity))
        return false;

    lastWrittenState.erase (identity);
    return true;
}

//==============================================================================
// Moves pre-5.0.0 state out of the settings document, one plugin at a time.
//
// Order matters and is the whole safety property: the file is written, then read
// back and checked, and only then is the settings key dropped. A migration that
// deletes first and writes second loses presets the moment a disk is full.
void IconMenu::migrateStateToVault()
{
    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);
    const auto vault = stateVault();

    int moved  = 0;
    int stayed = 0;

    for (const auto& plugin : activePluginList.getTypes())
    {
        MemoryBlock block;

        switch (lighthost::state::decodeLegacyState (store.readState (plugin), block))
        {
            case lighthost::state::DecodeResult::decoded:
                break;

            case lighthost::state::DecodeResult::nothingSaved:
                continue;                 // nothing was ever stored here

            case lighthost::state::DecodeResult::failed:
                ++stayed;                 // undecodable, and the only copy there is
                continue;
        }

        const auto identity = ChainStore::identityOf (plugin);

        if (vault.write (identity, block)
            && vault.read (identity).getSize() == block.getSize())
        {
            store.stageState (plugin, {});
            ++moved;
        }
        else
        {
            ++stayed;
        }
    }

    if (moved > 0)
    {
        store.commit();
        flushSettings (*settings, "moving plugin states out of the settings file");

        juce::Logger::writeToLog ("IconMenu: moved saved state for " + juce::String (moved)
                                  + " plugin(s) into " + vault.getDirectory().getFullPathName());
    }

    if (stayed > 0)
    {
        juce::Logger::writeToLog ("IconMenu: could not move saved state for "
                                  + juce::String (stayed) + " plugin(s); it stays in the "
                                  "settings file and is still loaded from there");

        status.report (juce::String (stayed) + " plugin state(s) could not be moved out of the "
                       "settings file. Nothing was lost - they are still loaded from it.");
    }
}

void IconMenu::reportStatus (const juce::String& message)
{
    // status.onChange already fans out to the tray tooltip and the Preferences
    // status row, so reporting is all this needs to do.
    status.report (message);
}

void IconMenu::refreshTooltip()
{
    const auto name = JUCEApplication::getInstance()->getApplicationName();

    if (! pendingLoads.empty())
    {
        setIconTooltip (name + " - loading...");
        return;
    }

    // The tooltip is the only surface that is always there, needs no layout, and
    // costs the user nothing to find.
    if (status.hasProblem())
    {
        setIconTooltip (name + " - " + status.mostRecent());
        return;
    }

    setIconTooltip (name);
}

//==============================================================================
void IconMenu::listenTo (AudioProcessor& processor)
{
    processor.addListener (this);
}

void IconMenu::stopListeningTo (NodeID nodeId)
{
    if (auto* node = graph.getNodeForId (nodeId))
        if (auto* processor = node->getProcessor())
            processor->removeListener (this);
}

void IconMenu::stopListeningToAll()
{
    for (auto* node : graph.getNodes())
        if (auto* processor = node->getProcessor())
            processor->removeListener (this);
}

void IconMenu::audioProcessorChanged (AudioProcessor*, const ChangeDetails& details)
{
    // Only latency matters for routing. A parameter or program change does not
    // move audio in time, so it needs no rewire.
    if (details.latencyChanged)
        triggerAsyncUpdate();
}

void IconMenu::audioProcessorParameterChanged (AudioProcessor*, int, float)
{
    // Deliberately empty. This fires for every automated parameter change, often
    // from the audio thread, and none of it affects how the graph is wired.
}

void IconMenu::handleAsyncUpdate()
{
    juce::Logger::writeToLog ("IconMenu: a plugin changed its reported latency; rewiring");
    reconnectGraph();

    // The number on the Preferences panel is now stale. smartChain re-declares
    // when its latency mode is switched, which is exactly the moment someone
    // wants to see what it cost.
    refreshPreferencesIfOpen();
}

//==============================================================================
void IconMenu::onAllPluginsLoaded (int generation)
{
    if (generation != pluginLoadGeneration)
        return;

    pendingLoads.clear();
    nextLoadIndex = 0;
    reconnectGraph();
    refreshTooltip();

    // The probes were just re-derived over the committed chain, so anything
    // the Preferences window is showing about that chain is now one edit
    // behind. This is the completion of the load an Apply started, and it is
    // the point at which the instances exist, so it is the right place for
    // the refresh rather than at the end of applyPluginChain.
    refreshPreferencesIfOpen();

    juce::Logger::writeToLog ("IconMenu: loadActivePlugins complete");

    if (applyInitiatedLoad)
    {
        applyInitiatedLoad = false;

        if (preferencesWindow != nullptr)
            preferencesWindow->setApplyFeedback ("Chain updated");
    }
}

//==============================================================================
void IconMenu::reconnectGraph()
{
    // NOTE ON PLUGIN DELAY COMPENSATION
    //
    // There is deliberately none here. juce::AudioProcessorGraph already performs
    // inter-lane latency compensation when it builds its render sequence: it
    // accumulates each node's getLatencySamples() along every path, takes the max
    // across the paths feeding a node, and inserts a delay op on every shorter
    // path (see RenderSequenceBuilder in juce_AudioProcessorGraph.cpp).
    //
    // Versions up to 4.0.3 also inserted their own DelayProcessor on shorter lanes.
    // Because that processor delayed audio without reporting the delay via
    // setLatencySamples, the graph still saw a zero-latency lane and padded it a
    // SECOND time. Measured: two lanes with 512 samples of latency difference
    // produced impulses at 512 and 1024 instead of one at 512. The compensation
    // caused exactly the misalignment it existed to prevent, so the host was worse
    // off than with no PDC at all. Tests/GraphRenderTests.cpp pins the correct
    // behaviour through the real graph.
    //
    // Do not reintroduce manual padding here without first making the padding node
    // report its own latency, and then checking whether it is needed at all.
    //
    // NOTE ON WHY THIS APPLIES A DIFF
    //
    // Up to 4.0.3 this function removed every connection in the graph and then
    // added the ones it wanted, each call defaulting to UpdateKind::sync. Every
    // one of those ~70 mutations per click published a new render sequence to the
    // audio thread, including the intermediate ones where the chain was partly or
    // entirely disconnected, which is audible as a click or a dropout on
    // something as small as a bypass toggle. The teardown also destroyed the
    // graph's own delay lines, so the compensation restarted from silence.
    //
    // Now the desired wiring is computed first (GraphTopology.hpp), diffed
    // against what the graph already holds, and only the difference is applied
    // with UpdateKind::none. A single rebuild() at the end publishes one
    // consistent sequence. A change that alters nothing applies nothing.

    // Before the layout, so the nodes referenced below exist.
    const auto retiredProbes = syncProbeNodes();

    lighthost::topology::Layout layout;
    layout.inputNodeId  = inputNodeId;
    layout.outputNodeId = outputNodeId;

    if (auto* inputNode = graph.getNodeForId (inputNodeId))
        if (auto* processor = inputNode->getProcessor())
            layout.inputNodeChannels = processor->getTotalNumOutputChannels();

    if (auto* outputNode = graph.getNodeForId (outputNodeId))
        if (auto* processor = outputNode->getProcessor())
            layout.outputNodeChannels = processor->getTotalNumInputChannels();

    // A lane whose trim is missing gets a zero id, which the topology reads as
    // "wire this lane straight to the output".
    for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
        if (graph.getNodeForId (laneGainNodeId (lane)) != nullptr)
            layout.laneGainNodeIds[static_cast<size_t> (lane)] = laneGainNodeId (lane);

    const ChainStore store (*getAppProperties().getUserSettings());

    int chainPosition = -1;

    const auto chain = getTimeSortedList();

    for (const auto& pd : *chain)
    {
        // Advanced for every entry in the chain as displayed, including the ones
        // skipped below, so it stays in step with the signal view's rows.
        ++chainPosition;

        const auto nodeIdVal = store.readNodeId (pd);
        if (nodeIdVal == 0)
            continue;

        const NodeID nodeId { static_cast<uint32> (nodeIdVal) };
        auto* node = graph.getNodeForId (nodeId);
        if (node == nullptr)
            continue;

        // Written only on a change. Node::setBypassed is unconditional, and for
        // a plugin that exposes a bypass parameter it routes to
        // setValueNotifyingHost, which is unconditional too -- so every device
        // change, latency report, bypass toggle, move, delete, Apply and
        // signal-view toggle pushed an automation-visible parameter change into
        // every hosted plugin, carrying the value each already had. Anything
        // recording automation from the plugin sees those.
        if (const auto wantBypass = store.readBypassed (pd); node->isBypassed() != wantBypass)
        {
            node->setBypassed (wantBypass);

            // Read back, and stop asking a node that will not take it.
            //
            // isBypassed() consults the plugin's own bypass PARAMETER, not our
            // stored intent, so a plugin that clamps or refuses the write leaves
            // the condition above true for ever and is written on every rewire.
            // Not a loop by itself -- parameter changes are deliberately ignored
            // here -- but an unbounded write into third-party code, and paired
            // with a plugin that re-declares latency it would close one.
            // Bounded and said out loud instead, like the sample-rate budget.
            if (node->isBypassed() != wantBypass
                && bypassRefused.insert (nodeId.uid).second)
            {
                juce::Logger::writeToLog ("IconMenu: " + pd.name
                                          + " will not accept a bypass write;"
                                            " not asking again this session");
            }
        }

        auto* processor = node->getProcessor();
        if (processor == nullptr)
            continue;

        lighthost::topology::NodeFacts facts;
        facts.nodeId            = nodeId;

        // Clamped rather than trusted: the lane comes straight from the
        // settings file and nothing between here and there validates it.
        // topology::buildConnections clamps again and is what actually holds
        // the guarantee, but OfflineRender.hpp clamps at its equivalent site
        // too, and this is the third copy of the same wiring rule -- the copies
        // drifting apart is what NodeIds.hpp exists to stop.
        facts.lane              = juce::jlimit (0, lighthost::kMaxLane, store.readLane (pd));
        facts.numInputChannels  = processor->getTotalNumInputChannels();
        facts.numOutputChannels = processor->getTotalNumOutputChannels();

        if (! lighthost::topology::canPassAudio (facts))
            juce::Logger::writeToLog ("IconMenu: " + pd.name + " has "
                                      + juce::String (facts.numInputChannels) + " in / "
                                      + juce::String (facts.numOutputChannels)
                                      + " out channels, so it cannot sit in a lane; wiring around it");

        // Indexed by position in the DISPLAYED chain, not by how many nodes made
        // it into the layout. Those differ the moment a plugin fails to load or
        // is still loading, and then every probe after it would be attributed to
        // the wrong plugin -- which for a column whose whole point is "which
        // plugin did that" is worse than showing nothing.
        if (signalViewEnabled
            && chainPosition < kMaxProbes
            && graph.getNodeForId (probeNodeId (chainPosition)) != nullptr)
        {
            facts.probeNodeId = probeNodeId (chainPosition);
        }

        layout.nodes.push_back (facts);
    }

    const auto desired = lighthost::topology::buildConnections (layout);
    const auto diff    = lighthost::topology::applyConnections (graph, desired);

    // One publish to the audio thread, whatever changed.
    graph.rebuild();

    // Cleared here rather than in syncProbeNodes, so the row of a probe that
    // has gone reads silence instead of the level frozen at the moment it went.
    // It has to be after the rebuild: until the new sequence is published the
    // old one is still feeding these meters, and Meter::publish would put the
    // level straight back. What this cannot cover is a block already in flight
    // when rebuild() returns -- the audio thread picks the new sequence up at
    // the start of its next callback -- so one block of stale level remains
    // possible. That is ~10 ms rather than the whole of the work above.
    for (const int index : retiredProbes)
        probeMeters[static_cast<size_t> (index)].reset();

    for (const auto& connection : diff.refused)
        juce::Logger::writeToLog ("IconMenu: graph refused connection "
                                  + juce::String ((int) connection.source.nodeID.uid) + ":"
                                  + juce::String (connection.source.channelIndex) + " -> "
                                  + juce::String ((int) connection.destination.nodeID.uid) + ":"
                                  + juce::String (connection.destination.channelIndex));

    if (diff.added > 0 || diff.removed > 0)
        juce::Logger::writeToLog ("IconMenu: graph rewired (" + juce::String (diff.added)
                                  + " added, " + juce::String (diff.removed) + " removed, "
                                  + juce::String ((int) desired.size()) + " total)");
}
void IconMenu::logAudioConfig (const juce::String& contextLabel) const
{
    const auto setup    = deviceManager.getAudioDeviceSetup();
    auto* device        = deviceManager.getCurrentAudioDevice();
    const auto driver   = deviceManager.getCurrentAudioDeviceType();

    const int activeIn  = setup.inputChannels.countNumberOfSetBits();
    const int activeOut = setup.outputChannels.countNumberOfSetBits();
    const int devIn     = device != nullptr ? device->getInputChannelNames().size()  : 0;
    const int devOut    = device != nullptr ? device->getOutputChannelNames().size() : 0;
    const double sr     = setup.sampleRate;
    const int bufSize   = setup.bufferSize;

    juce::Logger::writeToLog (
        "AudioConfig [" + contextLabel + "]: driver=" + driver
        + " input='" + setup.inputDeviceName + "' (active " + juce::String (activeIn)
        + "/" + juce::String (devIn) + " ch)"
        + " output='" + setup.outputDeviceName + "' (active " + juce::String (activeOut)
        + "/" + juce::String (devOut) + " ch)"
        + " sr=" + juce::String (sr, 0) + "Hz"
        + " buf=" + juce::String (bufSize) + " samples");
}

void IconMenu::autoMatchSampleRate()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    auto setup = deviceManager.getAudioDeviceSetup();

    // The attempt budget belongs to ONE device, and naming that device is the
    // half of it decide() cannot do: it is handed a rate and a list of rates,
    // never an identity. The reset it CAN see -- the device reporting a rate it
    // supports -- never fires for a device that has been given up on, because
    // the rate it is stuck at is by definition one that device says it does not
    // support. So the count stayed at the limit after device A exhausted it,
    // and device B plugged in afterwards, whose rates also exclude the current
    // rate, was given up on by its FIRST callback with zero attempts of its
    // own: no correction at all, and a log line blaming a driver that had not
    // yet been asked for anything.
    //
    // Type as well as name, because two drivers can present the same device
    // under the same name and they do not offer the same rates.
    if (sampleRateBudget.useDevice (deviceManager.getCurrentAudioDeviceType()
                                        + "/" + device->getName()))
    {
        juce::Logger::writeToLog ("IconMenu: sample-rate correction budget restored for "
                                  + device->getName());
    }

    const auto decision = lighthost::samplerate::decide (setup.sampleRate,
                                                         device->getAvailableSampleRates(),
                                                         sampleRateBudget.attempts());

    // Before the switch, so the attempt number logged below is the one just
    // spent rather than the one before it.
    sampleRateBudget.note (decision);

    switch (decision.action)
    {
        case lighthost::samplerate::Action::keepCurrentRate:
            return;

        case lighthost::samplerate::Action::giveUp:
            // Once per episode, and now actually once. The count is already at
            // the limit on this branch, so every later broadcast from the same
            // device lands here too -- the increment that used to sit here
            // bought nothing but an unbounded count, and the "logged once"
            // claim beside it was false for every repeat.
            if (sampleRateBudget.takeGiveUpLog())
                juce::Logger::writeToLog ("IconMenu: giving up on the sample rate after "
                                          + juce::String (lighthost::samplerate::kMaxCorrections)
                                          + " attempts. The driver keeps reporting "
                                          + juce::String (setup.sampleRate, 0)
                                          + "Hz, which it also says it does not support.");
            return;

        case lighthost::samplerate::Action::applyRate:
            juce::Logger::writeToLog ("IconMenu: sample rate " + juce::String (setup.sampleRate, 0)
                                      + "Hz is unsupported; asking for "
                                      + juce::String (decision.rate, 0) + "Hz (attempt "
                                      + juce::String (sampleRateBudget.attempts()) + ")");
            setup.sampleRate = decision.rate;

            // The refusal, in the device's own words, rather than discarded.
            // This is the one place that knows the rate could not be set, and
            // without it the symptom is a host running at a rate the device
            // says it does not support with nothing anywhere saying why. Every
            // other device call in this file already reports its error string.
            if (const auto error = deviceManager.setAudioDeviceSetup (setup, true);
                error.isNotEmpty())
            {
                juce::Logger::writeToLog ("IconMenu: the device refused "
                                          + juce::String (decision.rate, 0) + "Hz: " + error);
                status.report ("The audio device refused " + juce::String (decision.rate, 0)
                               + "Hz: " + error);
            }

            return;
    }
}

//==============================================================================
IconMenu::ChainSnapshot IconMenu::getTimeSortedList() const
{
    if (sortedPluginCache != nullptr)
        return sortedPluginCache;

    const ChainStore store (*getAppProperties().getUserSettings());
    const auto types = activePluginList.getTypes();

    // Pre-read every order value once — O(N) settings reads — so the sort
    // comparator uses O(1) integer comparisons instead of reading settings
    // and allocating strings on every comparison call (was O(N log N) reads).
    std::vector<std::pair<int, PluginDescription>> orderedTypes;
    orderedTypes.reserve (static_cast<size_t> (types.size()));
    for (int i = 0; i < types.size(); ++i)
        orderedTypes.emplace_back (store.readOrder (types[i]), types[i]);

    sortedPluginCache = std::make_shared<const std::vector<PluginDescription>> (
        ChainStore::sortByOrder (std::move (orderedTypes)));

    return sortedPluginCache;
}

void IconMenu::recordRequestedDevices (const juce::String& input, const juce::String& output)
{
    auto* settings = getAppProperties().getUserSettings();

    const auto record = lighthost::device::encodeRequested (
        lighthost::device::asRequest (input, output));

    settings->setValue (lighthost::keys::requestedDevices, record.get());

    // Flushed here rather than left to the caller. The write only matters
    // across a restart, and the run that most needs it is the one that ends
    // with the device gone and the process killed rather than quit.
    flushSettings (*settings, "recording the chosen audio devices");

    juce::Logger::writeToLog ("IconMenu: recorded requested devices, input='"
                              + input + "' output='" + output + "'");
}

void IconMenu::reportDeviceSubstitutionIfAny (const juce::String& contextLabel)
{
    auto* settings = getAppProperties().getUserSettings();

    // Two sources, in that order of preference; requestToCompare explains why.
    // The short version: DEVICESETUP stops being a record of the REQUEST the
    // moment anything calls updateXml(), which three sites in this application
    // do. So it is the fallback for installs with no record of their own yet,
    // not the source of truth it was in 5.2.0.
    const auto recorded = settings->getXmlValue (lighthost::keys::requestedDevices);
    const auto stored   = settings->getXmlValue (lighthost::keys::audioDeviceState);
    const auto setup    = deviceManager.getAudioDeviceSetup();

    const auto message = lighthost::device::describeSubstitution (
        lighthost::device::requestToCompare (recorded.get(), stored.get()),
        { setup.inputDeviceName, setup.outputDeviceName });

    if (! message.has_value())
        return;                 // the requested devices are the ones that are open

    // Deduplicated against what the status row is actually SHOWING, not against
    // a remembered string.
    //
    // The sink surfaces only its most recent problem, and both the tray tooltip
    // and the Preferences status row read that one string. Remembering the last
    // substitution instead meant a plugin-state failure arriving afterwards
    // replaced this message everywhere and the early return then stopped it ever
    // being said again -- leaving a substituted microphone with no indication
    // anywhere but the log. Comparing against the visible text repeats it when
    // something else has taken the row, and stays quiet while it is still up.
    if (*message == status.mostRecent())
        return;

    juce::Logger::writeToLog ("IconMenu: device substitution at " + contextLabel
                              + ": " + *message);
    reportStatus (*message);
}

//==============================================================================
void IconMenu::changeListenerCallback (ChangeBroadcaster* changed)
{
    auto* settings = getAppProperties().getUserSettings();

    if (changed == &knownPluginList)
    {
        if (auto xml = knownPluginList.createXml())
        {
            settings->setValue (lighthost::keys::pluginList, xml.get());
            flushSettings (*settings, "saving the scanned plugin list");
        }
    }
    else if (changed == &activePluginList)
    {
        // A throw from this write is unrecoverable, and is deliberately left to
        // terminate the process.
        //
        // The two mutation sites -- handleDeletePlugin and applyPluginChain --
        // wrap removeType/addType in a try/catch that rolls the settings back.
        // Neither guard reaches here: sendChangeMessage is an async update, so
        // the write lands on a later message-thread callback, by which time the
        // scope that owned the rollback has returned.
        //
        // Moving the persist inside those guards was the other option and is
        // refused, because it changes WHEN settings are written rather than
        // only where the guard sits. applyPluginChain mutates the list once per
        // arriving and departing plugin, so an inline write turns one save per
        // apply into one per plugin, and this listener would still run
        // afterwards and write again. Changing persistence timing to close a
        // comment gap is the wrong trade.
        //
        // Catching it here without a rollback would be worse than terminating.
        // The in-memory list has already changed; swallowing the failure leaves
        // a settings file describing a chain that is not the one running, and
        // the next launch restores that wrong chain with nothing to say so. A
        // crash leaves the previous settings file intact and correct.
        //
        // In practice nothing here throws: createXml and setValue allocate, and
        // flushSettings reports a failed write rather than throwing. This is a
        // statement of what a std::bad_alloc would do, not a live hazard.
        if (auto xml = activePluginList.createXml())
        {
            settings->setValue (lighthost::keys::pluginListActive, xml.get());
            flushSettings (*settings, "saving the plugin chain");
        }
    }
    else if (changed == &deviceManager)
    {
        // JUCE marshals ChangeBroadcaster callbacks to the message thread; assert to
        // catch any future regression that would make reconnectGraph() unsafe here.
        JUCE_ASSERT_MESSAGE_THREAD;

        if (isHandlingDeviceChange)
            return;

        // Covers a synchronous re-entry only, and cheaply. The recursion that
        // matters is asynchronous and is bounded by the attempt counter inside
        // autoMatchSampleRate instead. ScopedValueSetter clears the flag even if
        // something below throws, so a throw cannot permanently silence future
        // device changes.
        const juce::ScopedValueSetter<bool> guard (isHandlingDeviceChange, true, false);

        reconnectGraph();
        autoMatchSampleRate();
        juce::Logger::writeToLog ("IconMenu: audio device change handled for type "
                                  + deviceManager.getCurrentAudioDeviceType());
        logAudioConfig ("device-change");

        // Before the write below, which is what the comparison is against.
        reportDeviceSubstitutionIfAny ("device-change");

        if (auto xml = deviceManager.createStateXml())
        {
            settings->setValue (lighthost::keys::audioDeviceState, xml.get());
            flushSettings (*settings, "saving the audio device settings");
        }
    }
}

//==============================================================================
void IconMenu::timerCallback()
{
    stopTimer();
    menu.clear();
    menu.addSectionHeader (JUCEApplication::getInstance()->getApplicationName());

    menu.addItem (1, "Preferences");
    menu.addItem (2, "Edit Plugins");
    menu.addSeparator();
    menu.addSectionHeader ("Active Plugins");

    const auto timeSortedSnapshot = getTimeSortedList();
    const auto& timeSorted = *timeSortedSnapshot;

    const ChainStore store (*getAppProperties().getUserSettings());

    for (size_t i = 0; i < timeSorted.size(); ++i)
    {
        PopupMenu options;
        options.addItem (kEditOffset + static_cast<int> (i), "Edit");

        options.addItem (kBypassOffset + static_cast<int> (i), "Bypass", true,
                         store.readBypassed (timeSorted[i]));
        options.addSeparator();
        options.addItem (kMoveUpOffset + static_cast<int> (i), "Move Up", i > 0);
        options.addItem (kMoveDownOffset + static_cast<int> (i), "Move Down", i < timeSorted.size() - 1);
        options.addSeparator();
        options.addItem (kDeleteOffset + static_cast<int> (i), "Delete");

        menu.addSubMenu (timeSorted[i].name, options);
    }

    // Quit goes last, and Delete Plugin States is kept two rows and a separator
    // away from it. The two used to be adjacent, with the destructive one
    // underneath, so an over-shoot on the way to Quit landed on "throw away
    // every plugin setting" -- which was nearly clicked three times before this
    // was changed. Nothing below Quit, and nothing destructive beside it.
    menu.addSeparator();
    menu.addItem (4, "Delete Plugin States");
    #if JUCE_WINDOWS
    menu.addItem (5, "Run at Startup", true, isStartupEnabled());
    #endif
    menu.addSeparator();
    menu.addItem (3, "Quit");

    // Negative form on purpose. The #else reaches GetCursorPos, which exists
    // only on Windows, so every platform that is not Windows belongs above --
    // spelled as a list, this said "JUCE_MAC || JUCE_LINUX" and sent BSD into
    // the Win32 branch, where it could not compile. The confirmation dialog two
    // functions down had already been fixed to handle JUCE_BSD; this had not,
    // so the two disagreed about which platforms exist.
    #if ! JUCE_WINDOWS
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (this),
                        ModalCallbackFunction::forComponent (menuInvocationCallback, this));
    #else
    POINT iconLocation = { 0, 0 };
    GetCursorPos (&iconLocation);

    juce::Rectangle<int> rect (iconLocation.x, iconLocation.y, 1, 1);
    menu.showMenuAsync (PopupMenu::Options().withTargetScreenArea (rect),
                        ModalCallbackFunction::forComponent (menuInvocationCallback, this));
    #endif
}

//==============================================================================
void IconMenu::mouseDown (const MouseEvent& e)
{
    #if JUCE_MAC
    Process::setDockIconVisible (true);
    #endif

    Process::makeForegroundProcess();

    if (e.mods.isLeftButtonDown())
    {
        showPreferences();
        return;
    }

    startTimer (50);
}

//==============================================================================
void IconMenu::menuInvocationCallback (int id, IconMenu* im)
{
    if (id == 0)
    {
        #if JUCE_MAC
        if (! PluginWindow::containsActiveWindows())
            Process::setDockIconVisible (false);
        #endif
        return;
    }

    // Fixed items
    if (id == 1) { im->showPreferences(); return; }
    if (id == 2) { im->reloadPlugins();   return; }
    if (id == 3) { im->savePluginStates(); JUCEApplication::getInstance()->quit(); return; }
    if (id == 4) { im->confirmDeletePluginStates(); return; }
    #if JUCE_WINDOWS
    if (id == 5) { im->setStartupEnabled (! im->isStartupEnabled()); return; }
    #endif

    // Plugin actions.
    //
    // The width of each band is kMenuBandStride, not kEditOffset. These read
    // "id < kDeleteOffset + kEditOffset" and worked only because kEditOffset
    // happens to equal the spacing between the offsets: respace them, or insert
    // a sixth action, and the bands overlap so a Move Up click dispatches to
    // handleDeletePlugin. Silent, and destructive.
    const auto inBand = [id] (int offset)
    {
        return juce::isPositiveAndBelow (id - offset, kMenuBandStride);
    };

    if (inBand (kDeleteOffset))
        im->handleDeletePlugin (id - kDeleteOffset);
    else if (inBand (kMoveDownOffset))
        im->handleMovePlugin (id - kMoveDownOffset, false);
    else if (inBand (kMoveUpOffset))
        im->handleMovePlugin (id - kMoveUpOffset, true);
    else if (inBand (kBypassOffset))
        im->handleBypassPlugin (id - kBypassOffset);
    else if (inBand (kEditOffset))
        im->handleEditPlugin (id - kEditOffset);

    im->startTimer (50);
}

//==============================================================================
void IconMenu::handleDeletePlugin (int index)
{
    const auto timeSorted = getTimeSortedList();

    // juce::isPositiveAndBelow rather than a hand-written pair of comparisons.
    // This bounds check was spelled out six times in this file, and BACKLOG.md
    // wanted regression tests for the four in the handleXxx methods -- which
    // would have meant testing a local copy of an expression JUCE already
    // provides and tests. One library call per site leaves nothing bespoke to
    // regress.
    if (! juce::isPositiveAndBelow (index, timeSorted->size()))
        return;

    const auto pluginToDelete = (*timeSorted)[static_cast<size_t> (index)];
    juce::Logger::writeToLog ("IconMenu: deleting plugin " + pluginToDelete.name);

    cancelPluginLoading();

    // Save surviving plugin states before we discard the deleted plugin's node.
    savePluginStates();

    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);

    const auto nodeIdVal = store.readNodeId (pluginToDelete);

    // Erase every field the store owns, which is the point of it owning them:
    // up to 4.0.3 this list was written out by hand here and omitted the lane, so
    // a delete left an orphan lane key for the next plugin to inherit.
    store.stageErase (pluginToDelete);

    // Read here, used after the list mutation below. The state file and the
    // in-memory copy this names are the only irreversible things this function
    // does; see the erases themselves for why they cannot happen yet.
    const auto deletedIdentity = ChainStore::identityOf (pluginToDelete);

    // Remove from the list first, so nothing destructive happens until it has
    // succeeded.
    //
    // Note what this try/catch does NOT cover, because the comment here used to
    // claim otherwise: the change listener that writes the XML does not run
    // inside it. ChangeBroadcaster::sendChangeMessage is an async update, so the
    // listener -- and the settings write it performs -- happens later on the
    // message thread, outside this scope. removeType itself only takes a lock
    // and mutates an Array, so in practice there is nothing here to throw. The
    // guard is kept because a rollback is the correct response if it ever does.
    // What happens if the listener's own write throws is settled where it
    // happens, in changeListenerCallback: unrecoverable, and left to terminate.
    try
    {
        activePluginList.removeType (pluginToDelete);  // triggers changeListener → persists XML
    }
    catch (const std::exception& e)
    {
        store.rollback();
        sortedPluginCache.reset();   // as applyPluginChain's abandon path does
        juce::Logger::writeToLog ("activePluginList.removeType threw std::exception for "
                                  + pluginToDelete.name + ": " + juce::String (e.what())
                                  + "; the plugin's settings were left untouched");
        refreshPreferencesIfOpen();
        return;
    }
    catch (...)
    {
        store.rollback();
        sortedPluginCache.reset();
        juce::Logger::writeToLog ("activePluginList.removeType threw unknown exception for "
                                  + pluginToDelete.name
                                  + "; the plugin's settings were left untouched");
        refreshPreferencesIfOpen();
        return;
    }

    // The state file and the in-memory copy go with the settings keys, exactly
    // as applyPluginChain does it for a departing plugin. This path did only
    // the stageErase above, which cost two things:
    //
    // The .lhs file was orphaned permanently. Nothing enumerates the vault
    // directory, so no sweep could ever collect it, and a real plugin state
    // here measured four megabytes.
    //
    // Worse, lastWrittenState kept the bytes too, so re-adding the same plugin
    // later RESTORED the preset the user had deleted -- while deleting the
    // same plugin from the Preferences list did not. Two ways to remove a
    // plugin, two different meanings, and the difference only showed up on
    // the next add.
    //
    // HERE, after the list mutation, rather than before the try above, which is
    // where they were. Erasing the state first and then throwing out of
    // removeType left the catch rolling the settings keys BACK over a state
    // file that was already gone: the plugin returned to the chain with its
    // saved preset silently replaced by factory defaults, which is worse than
    // either outcome the rollback is choosing between. These two are the only
    // irreversible steps in this function, so they go last, once nothing that
    // can fail is left.
    if (! forgetPluginState (stateVault(), deletedIdentity))
        status.report ("Removed " + pluginToDelete.name + " from the chain, but could not"
                       " delete its saved settings. Re-adding it will bring them back.");

    if (nodeIdVal != 0)
    {
        const NodeID nodeId { static_cast<uint32> (nodeIdVal) };
        stopListeningTo (nodeId);
        PluginWindow::closeCurrentlyOpenWindowsFor (nodeId);  // close UI first

        // UpdateKind::none, not the default sync. A sync removal disconnects the
        // node and publishes a render sequence with the lane cut, and the wiring
        // is not restored until reconnectGraph() below -- which is AFTER
        // store.commit() and flushSettings(), i.e. after a whole XML settings
        // document has been written to disk. Delete the only plugin in the chain
        // and the output was silent for the length of that write. The wiring is
        // recomputed a few lines down regardless, so nothing is lost by not
        // publishing here.
        //
        // The node survives the removal: the live render sequence holds a
        // Node::Ptr to it (juce_AudioProcessorGraph.cpp, NodeOp), so the instance
        // is destroyed only when that sequence is replaced.
        //
        // stopListeningTo and closeCurrentlyOpenWindowsFor above need no
        // equivalent: neither touches the graph topology, so neither publishes
        // anything to the audio thread.
        graph.removeNode (nodeId, juce::AudioProcessorGraph::UpdateKind::none);
    }

    store.commit();
    flushSettings (*settings, "deleting a plugin");

    sortedPluginCache.reset();
    reconnectGraph();  // rewire around the removed node — no plugin loads needed
    refreshPreferencesIfOpen();
}

void IconMenu::handleBypassPlugin (int index)
{
    const auto timeSorted = getTimeSortedList();

    if (! juce::isPositiveAndBelow (index, timeSorted->size()))
        return;

    const auto plugin = (*timeSorted)[static_cast<size_t> (index)];

    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);

    store.stageBypassed (plugin, ! store.readBypassed (plugin));
    store.commit();
    flushSettings (*settings, "toggling bypass");

    reconnectGraph();
    refreshPreferencesIfOpen();
}

void IconMenu::handleEditPlugin (int index)
{
    const auto sortedSnapshot = getTimeSortedList();
    const auto& sorted = *sortedSnapshot;

    if (! juce::isPositiveAndBelow (index, sorted.size()))
        return;

    const ChainStore store (*getAppProperties().getUserSettings());
    const auto nodeIdVal = store.readNodeId (sorted[static_cast<size_t> (index)]);
    if (nodeIdVal == 0)
        return;

    juce::Logger::writeToLog ("IconMenu: opening editor for " + sorted[static_cast<size_t> (index)].name);

    if (auto* f = graph.getNodeForId (NodeID { static_cast<uint32> (nodeIdVal) }))
        if (auto* w = PluginWindow::getWindowFor (f, PluginWindow::Normal))
            w->toFront (true);
}

void IconMenu::handleMovePlugin (int index, bool moveUp)
{
    const auto timeSortedSnapshot = getTimeSortedList();
    const auto& timeSorted = *timeSortedSnapshot;

    if (! juce::isPositiveAndBelow (index, timeSorted.size()))
        return;

    const int neighborIndex = moveUp ? index - 1 : index + 1;

    if (! juce::isPositiveAndBelow (neighborIndex, timeSorted.size()))
        return;

    const auto target   = timeSorted[static_cast<size_t> (index)];
    const auto neighbor = timeSorted[static_cast<size_t> (neighborIndex)];

    // Swap only the two affected order values — O(1) writes.  Previously
    // rewrote all N values and called savePluginStates() even though plugin
    // state is unchanged by a move.
    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);

    const auto targetOrder   = store.readOrder (target);
    const auto neighborOrder = store.readOrder (neighbor);

    store.stageOrder (target,   neighborOrder);
    store.stageOrder (neighbor, targetOrder);
    store.commit();
    flushSettings (*settings, "reordering the chain");

    sortedPluginCache.reset();
    reconnectGraph();
    refreshPreferencesIfOpen();
}

//==============================================================================
void IconMenu::confirmDeletePluginStates()
{
    // Erasing every saved plugin setting was a single click with no undo, sat
    // directly below Quit in the tray menu. The menu order has been fixed; this
    // fixes the other half, because proximity was only half the problem.
    //
    // The dialog names the plugins rather than asking "are you sure?", since a
    // generic question is easy to click through and the cost here is real: a
    // trained curve that took a while to teach is gone with no way back.
    const auto listSnapshot = getTimeSortedList();
    const auto& list = *listSnapshot;

    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);
    const auto vault = stateVault();

    juce::StringArray named;

    for (const auto& plugin : list)
    {
        const auto identity = ChainStore::identityOf (plugin);

        // Either store counts: the vault is where states live now, and a legacy
        // blob in the settings file is one that has not been migrated yet.
        if (vault.has (identity) || store.readState (plugin).isNotEmpty())
            named.add (plugin.name);
    }

    if (named.isEmpty())
    {
        reportStatus ("No saved plugin states to delete");
        return;
    }

    // Which button goes where, and which index means "do it", both come from
    // Source/ConfirmPolicy.hpp -- the platforms disagree about what a dismissed
    // dialog reports, and they disagree in opposite directions. It is a pure
    // function so CI checks both rules on every platform, rather than the rule
    // being confirmable only by a person clicking a dialog on a Linux box.
    const auto order = lighthost::confirm::buttonOrder (
        lighthost::confirm::thisPlatform(), "Delete", "Cancel");

    const auto deleteIndex = lighthost::confirm::destructiveIndex (order, "Delete");

    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::WarningIcon)
                       .withTitle ("Delete Plugin States")
                       .withMessage ("This erases the saved settings for "
                                     + juce::String (named.size())
                                     + (named.size() == 1 ? " plugin:\n\n"
                                                          : " plugins:\n\n")
                                     + named.joinIntoString ("\n")
                                     + "\n\nEach one goes back to its factory state. "
                                       "This cannot be undone.");

    for (const auto& text : order)
        options = options.withButton (text);

    juce::NativeMessageBox::showAsync (
        options,
        [safe = juce::Component::SafePointer<IconMenu> (this), deleteIndex] (int result)
        {
            if (result != deleteIndex || safe == nullptr)
                return;

            const auto failed = safe->deletePluginStates();
            safe->loadActivePlugins();

            // Says what happened, which the old unconditional "Deleted saved
            // plugin states" could not: it was true of a run in which every
            // single delete failed.
            if (failed == 0)
                safe->reportStatus ("Deleted saved plugin states");
            else
                safe->reportStatus ("Could not delete " + juce::String (failed)
                                    + (failed == 1 ? " plugin's saved state."
                                                   : " plugins' saved states.")
                                    + " Something else is holding the files open.");
        });
}

int IconMenu::deletePluginStates()
{
    const auto listSnapshot = getTimeSortedList();
    const auto& list = *listSnapshot;

    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);

    const auto vault = stateVault();

    int failed = 0;

    for (const auto& plugin : list)
    {
        const auto identity = ChainStore::identityOf (plugin);

        store.stageState (plugin, {});   // clears any un-migrated legacy blob

        if (forgetPluginState (vault, identity))
            continue;

        ++failed;
        juce::Logger::writeToLog ("Could not delete saved state for " + plugin.name);
    }

    store.commit();
    flushSettings (*settings, "clearing saved plugin states");
    return failed;
}

void IconMenu::savePluginStates()
{
    const auto listSnapshot = getTimeSortedList();
    const auto& list = *listSnapshot;

    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);
    const auto vault = stateVault();

    for (const auto& plugin : list)
    {
        const auto nodeIdVal = store.readNodeId (plugin);
        if (nodeIdVal == 0)
            continue;

        // A plugin whose saved state failed to restore this session still holds
        // its factory defaults. Saving those would overwrite the good blob on
        // disk, turning a transient load failure into permanent data loss, so
        // leave the stored state untouched.
        if (statesNotRestored.count (static_cast<uint32> (nodeIdVal)) != 0)
        {
            juce::Logger::writeToLog ("Skipping state save for " + plugin.name
                                      + ": its saved state never restored this session");
            continue;
        }

        auto* node = graph.getNodeForId (NodeID { static_cast<uint32> (nodeIdVal) });
        if (node == nullptr)
            continue;

        try
        {
            MemoryBlock savedStateBinary;
            node->getProcessor()->getStateInformation (savedStateBinary);

            // Nothing to save is not the same as "save nothing", and it is not
            // an error either. Plenty of plugins have no state at all and hand
            // back an empty block every time, so reporting this would put a
            // failure in front of the user on every chain edit. The plugins that
            // DO have state can also return empty transiently, and the rule that
            // covers both is the same one statesNotRestored follows above: leave
            // what is on disk alone. Vault::write refuses an empty block now, so
            // this only decides whether the user hears about it.
            if (savedStateBinary.getSize() == 0)
                continue;

            const auto identity    = ChainStore::identityOf (plugin);
            const auto print       = lighthost::state::Vault::fingerprint (savedStateBinary);
            const auto alreadyHere = lastWrittenState.find (identity);

            // Unchanged bytes are not rewritten. Before 5.0.0 every save wrote
            // every plugin, so one bypass toggle rewrote the whole document;
            // now a save that changed nothing touches no files at all.
            if (alreadyHere != lastWrittenState.end()
                && alreadyHere->second == print
                && vault.has (identity))
                continue;

            if (vault.write (identity, savedStateBinary))
            {
                lastWrittenState[identity] = print;

                // The pre-5.0.0 blob is only dropped once its replacement is on
                // disk, so an interrupted upgrade never leaves neither copy.
                store.stageState (plugin, {});
            }
            else
            {
                juce::Logger::writeToLog ("Could not write state file for " + plugin.name);
                status.report ("Could not save settings for " + plugin.name
                               + ". Its previously saved settings are unchanged.");
            }
        }
        catch (const std::exception& e)
        {
            juce::Logger::writeToLog ("Plugin state save threw std::exception for " + plugin.name + ": " + e.what());
        }
        catch (...)
        {
            juce::Logger::writeToLog ("Plugin state save threw unknown exception for " + plugin.name);
        }
    }

    try
    {
        // A plugin that threw above simply has nothing staged, so the state
        // already on disk survives; the others are written together.
        store.commit();
        flushSettings (*settings, "saving plugin states");
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog ("Settings saveIfNeeded threw std::exception in savePluginStates: "
                                  + juce::String (e.what()));
    }
    catch (...)
    {
        juce::Logger::writeToLog ("Settings saveIfNeeded threw unknown exception in savePluginStates");
    }
}

//==============================================================================
void IconMenu::showPreferencesWindow()
{
    showPreferences();
}

void IconMenu::showPreferences()
{
    if (preferencesWindow != nullptr)
    {
        preferencesWindow->toFront (true);
        return;
    }

    juce::Logger::writeToLog ("IconMenu: opening Preferences window");

    Component::SafePointer<IconMenu> safe (this);

    // Build initial bypass and lane states from persisted settings
    const auto chainSnapshot = getTimeSortedList();
    const auto& chain = *chainSnapshot;

    const ChainStore store (*getAppProperties().getUserSettings());

    std::vector<bool> bypassStates;
    std::vector<int>  laneStates;
    bypassStates.reserve (chain.size());
    laneStates.reserve (chain.size());

    for (const auto& plugin : chain)
    {
        bypassStates.push_back (store.readBypassed (plugin));
        laneStates.push_back (store.readLane (plugin));
    }

    PreferencesWindow::LaneTrim laneTrim;

    for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
        laneTrim.initialDb[static_cast<size_t> (lane)] = store.readLaneGainDb (lane);

    laneTrim.onChanged = [safe] (int lane, float decibels)
    {
        if (auto* im = safe.getComponent())
            im->setLaneGainDb (lane, decibels);
    };

    laneTrim.onCommitted = [safe] (int lane, float decibels)
    {
        if (auto* im = safe.getComponent())
            im->persistLaneGainDb (lane, decibels);
    };

    preferencesWindow = std::make_unique<PreferencesWindow> (
        deviceManager,
        knownPluginList,
        chain,
        bypassStates,
        laneStates,
        std::move (laneTrim),
        [safe] (const std::vector<PluginDescription>& newChain,
                const std::vector<bool>& newBypass,
                const std::vector<int>& newLanes)
        {
            if (auto* im = safe.getComponent())
                im->applyPluginChain (newChain, newBypass, newLanes);
        },
        [safe] (const PluginDescription& pd)
        {
            if (auto* im = safe.getComponent())
                im->openPluginEditorFor (pd);
        },
        [safe]() -> int
        {
            // Whatever the graph currently declares. Read for display only:
            // this is inherent plugin delay on a live path, so there is nothing
            // for the host to compensate and nothing for the user to set.
            if (auto* im = safe.getComponent())
                return im->graph.getLatencySamples();

            return 0;
        },
        &getInputMeter(),
        &getOutputMeter(),
        [safe] (bool enabled)
        {
            // The probes are created here, before the window fills its column
            // from them.
            if (auto* im = safe.getComponent())
                im->setSignalViewEnabled (enabled);
        },
        [safe] (int index) -> lighthost::metering::Meter*
        {
            if (auto* im = safe.getComponent())
                return im->getProbeMeter (index);

            return nullptr;
        },
        [safe]() -> std::vector<juce::String>
        {
            if (auto* im = safe.getComponent())
                return im->getCommittedChainNames();

            return {};
        },
        [safe] (const juce::String& inputName, const juce::String& outputName)
        {
            // Only Apply reaches here, which is the whole point: this is the
            // one moment we know a device name came from the user rather than
            // from JUCE settling on a fallback.
            if (auto* im = safe.getComponent())
                im->recordRequestedDevices (inputName, outputName);
        },
        [safe]()
        {
            if (auto* im = safe.getComponent())
            {
                juce::Logger::writeToLog ("IconMenu: closing Preferences window");

                // Otherwise signalViewEnabled stays true and every later rewire
                // keeps splicing probes into the graph for the rest of the
                // session -- which is the opposite of what this feature
                // promises, and leaves a reopened window showing the toggle off
                // while the nodes are still in there.
                im->setSignalViewEnabled (false);

                // Before the write below, for the same reason as the other two
                // sites: the comparison is against the stored request, and this
                // overwrites it.
                im->reportDeviceSubstitutionIfAny ("preferences-close");

                // Persist audio device state when the window is closed
                auto audioState = im->deviceManager.createStateXml();
                if (audioState != nullptr)
                {
                    auto* settings = getAppProperties().getUserSettings();
                    settings->setValue (lighthost::keys::audioDeviceState, audioState.get());
                    im->flushSettings (*settings, "closing Preferences");
                }

                // Defer the reset to avoid destroying the PreferencesWindow —
                // and its owning std::function — while still executing inside it.
                // callAsync posts after the current call stack fully unwinds.
                juce::MessageManager::callAsync ([safe]
                {
                    if (auto* im2 = safe.getComponent())
                        im2->preferencesWindow.reset();
                });
            }
        });

    // Whatever has already gone wrong is shown as soon as the window opens, not
    // only when the next thing goes wrong.
    preferencesWindow->setStatusMessage (status.mostRecent());
}

void IconMenu::refreshPreferencesIfOpen()
{
    if (preferencesWindow == nullptr) return;

    const auto chainSnapshot = getTimeSortedList();
    const auto& chain = *chainSnapshot;

    const ChainStore store (*getAppProperties().getUserSettings());

    std::vector<bool> bypass;
    std::vector<int>  lanes;
    bypass.reserve (chain.size());
    lanes.reserve (chain.size());

    for (const auto& plugin : chain)
    {
        bypass.push_back (store.readBypassed (plugin));
        lanes.push_back (store.readLane (plugin));
    }

    preferencesWindow->refreshPluginChain (chain, bypass, lanes);
}

void IconMenu::openPluginEditorFor (const PluginDescription& pd)
{
    const ChainStore store (*getAppProperties().getUserSettings());
    const auto nodeIdVal = store.readNodeId (pd);

    if (nodeIdVal == 0)
    {
        juce::Logger::writeToLog ("IconMenu: editor open skipped, missing nodeId for " + pd.name);
        return;
    }

    if (auto* f = graph.getNodeForId (NodeID { static_cast<uint32> (nodeIdVal) }))
    {
        if (auto* w = PluginWindow::getWindowFor (f, PluginWindow::Normal))
            w->toFront (true);
    }
    else
    {
        juce::Logger::writeToLog ("IconMenu: editor open skipped, node not loaded for " + pd.name);
    }
}

void IconMenu::applyPluginChain (const std::vector<PluginDescription>& newChain,
                                  const std::vector<bool>& bypassStates,
                                  const std::vector<int>& lanes)
{
    // One definition of "the same plugin", the same one the settings store uses,
    // so a plugin matched here is the plugin whose settings are read there.
    const auto identity = [] (const PluginDescription& pd) { return ChainStore::identityOf (pd); };

    const auto sameAsAny = [&identity] (const auto& haystack, const PluginDescription& needle)
    {
        return std::any_of (haystack.begin(), haystack.end(),
                            [&] (const PluginDescription& candidate)
                            { return identity (candidate) == identity (needle); });
    };

    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);

    const auto currentChainSnapshot = getTimeSortedList();
    const auto& currentChain = *currentChainSnapshot;

    // Membership, order, bypass and lane are all compared: a lane change alone
    // flips neither the order nor the bypass, and Apply used to no-op when only
    // the lane dropdown had moved.
    if (lighthost::chain::isNoOpEdit (store.entriesFor (currentChain),
                                      ChainStore::entriesFor (newChain, bypassStates, lanes)))
    {
        juce::Logger::writeToLog ("IconMenu: Apply pressed with no plugin-chain changes");

        // The chain is unchanged, but a plugin's own parameters may well not be.
        // A user who moves a knob inside a plugin editor and presses Apply
        // expects it kept, and this path used to return before saving anything --
        // so that edit lived only in the running instance and was lost to
        // anything short of a clean quit. It cost an afternoon: a plugin setting
        // was changed, Apply was pressed, and every measurement afterwards was
        // taken against the previous state still sitting in the vault.
        //
        // Saving unconditionally is cheap here because savePluginStates
        // fingerprints each state and skips the ones whose bytes have not moved,
        // so an Apply that really changed nothing writes nothing.
        savePluginStates();

        // Note the wording: device settings are committed by the caller before
        // this runs, so the chain is the only thing that was unchanged.
        if (preferencesWindow != nullptr)
            preferencesWindow->setApplyFeedback ("Plugin settings saved");

        return;
    }

    // Snapshot the current active list before mutations so iterators stay valid.
    const auto currentTypes = activePluginList.getTypes();

    // ── Work out what is leaving and what is arriving ────────────────────────
    // The node ids of departing plugins are read before anything is staged: once
    // an erase is staged the store reports them as gone, which is the point.
    struct Departing
    {
        PluginDescription description;
        int               nodeId;
    };

    std::vector<Departing> departing;
    std::vector<PluginDescription> arriving;

    for (const auto& existing : currentTypes)
        if (! sameAsAny (newChain, existing))
            departing.push_back ({ existing, store.readNodeId (existing) });

    for (const auto& candidate : newChain)
        if (! sameAsAny (currentTypes, candidate))
            arriving.push_back (candidate);

    if (! departing.empty() || ! arriving.empty())
        cancelPluginLoading();

    juce::Logger::writeToLog ("IconMenu: applying plugin chain (" + juce::String ((int) currentChain.size())
                              + " -> " + juce::String ((int) newChain.size()) + " plugins, "
                              + juce::String ((int) arriving.size()) + " added, "
                              + juce::String ((int) departing.size()) + " removed)");

    // Persist current plugin states before making any graph changes.
    savePluginStates();

    // ── Mutate the plugin list ───────────────────────────────────────────────
    // Nothing is written to the settings and no node is destroyed until every
    // list mutation has succeeded. A throw here leaves the settings exactly as
    // they were rather than describing a chain the user did not ask for.
    //
    // The listener's own XML write is outside this, and outside any guard --
    // see changeListenerCallback for why that is deliberate.
    const auto abandon = [&] (const juce::String& what, const juce::String& detail)
    {
        store.rollback();
        juce::Logger::writeToLog ("IconMenu: " + what + " threw (" + detail
                                  + "); chain edit abandoned. Staged settings were"
                                    " rolled back and no plugin state was deleted;"
                                    " the chain XML the change listener writes is"
                                    " outside this guard by design");
        sortedPluginCache.reset();
        reconnectGraph();
        refreshPreferencesIfOpen();
    };

    const auto vault = stateVault();

    // State files to delete, collected rather than deleted as we go, and applied
    // only once store.commit() has succeeded at the bottom of this function.
    //
    // Deleting in the loop was correct within one iteration -- the 5.4.0 change
    // that moved each erase after its guarded mutation -- but not across the
    // transaction. With two departing plugins and a throw on the second,
    // abandon() rolls the settings back, and rollback clears pendingWrites and
    // pendingRemovals; it cannot put the first plugin's .lhs back. The result
    // was settings describing a plugin whose preset no longer existed, and the
    // loop's own log line promising "re-adding it will restore the old preset"
    // was then false.
    //
    // Collecting them makes the erase part of the same all-or-nothing step as
    // the settings write, without redesigning ChainStore's staging: abandon()
    // returns before this list is ever walked, so nothing is deleted.
    std::vector<std::pair<juce::String, juce::String>> statesToForget;   // identity, name

    for (const auto& plugin : departing)
    {
        store.stageErase (plugin.description);

        const auto departingIdentity = ChainStore::identityOf (plugin.description);

        try
        {
            activePluginList.removeType (plugin.description);  // changeListener persists the XML
        }
        catch (const std::exception& e)
        {
            abandon ("activePluginList.removeType", plugin.description.name + ": " + e.what());
            return;
        }
        catch (...)
        {
            abandon ("activePluginList.removeType", plugin.description.name);
            return;
        }

        // AFTER the guarded mutation, not before it. abandon() rolls the
        // settings back, and deleting the state file is the one step in this
        // loop it cannot undo -- so doing it first meant a throw left the
        // preset gone with the settings restored, describing a plugin whose
        // saved state no longer exists. handleDeletePlugin had the identical
        // inversion and was fixed in the same pass.
        //
        // The state file still goes with the settings keys, or the directory
        // accumulates orphans for plugins no longer in the chain -- but it goes
        // WITH them, at the commit, not here. See statesToForget above.
        statesToForget.emplace_back (departingIdentity, plugin.description.name);
    }

    // Set when addType reported a replacement rather than an add. See the
    // reconciliation below for what that means and why it is not ignorable.
    bool anyAddReplaced = false;

    for (const auto& plugin : arriving)
    {
        if (store.readNodeId (plugin) == 0)
        {
            const auto allocated = store.allocateNodeId();

            if (allocated == 0)
            {
                status.report ("Ran out of plugin node ids, so " + plugin.name
                               + " was not added.");
                continue;
            }

            store.stageNodeId (plugin, allocated);
        }

        try
        {
            // The return value is "added", and false means addType REPLACED an
            // existing entry instead (juce_KnownPluginList.cpp:110, `desc =
            // type`). That is reachable because the two definitions of "the
            // same plugin" disagree: ChainStore::identityOf includes
            // pluginFormatName, PluginDescription::isDuplicateOf compares only
            // fileOrIdentifier, uniqueId and deprecatedUid. Two descriptions
            // sharing a file and ids under different format names are
            // therefore two identities and one list entry.
            if (! activePluginList.addType (plugin))  // changeListener persists the XML
                anyAddReplaced = true;
        }
        catch (const std::exception& e)
        {
            abandon ("activePluginList.addType", plugin.name + ": " + e.what());
            return;
        }
        catch (...)
        {
            abandon ("activePluginList.addType", plugin.name);
            return;
        }
    }

    // ── Reconcile the list against what was asked for ────────────────────────
    // Discarding the addType result left the chain one plugin shorter than the
    // user asked for, with a full set of settings behind for the entry that
    // lost -- keys and a state file belonging to nothing, invisible unless the
    // settings file is read by hand, and silently dropped from the chain on
    // every launch after.
    //
    // Done after the whole batch, not per add, because which description
    // survives depends on the order the adds ran in.
    std::vector<PluginDescription> dropped;

    if (anyAddReplaced)
    {
        const auto finalTypes = activePluginList.getTypes();

        for (const auto& wanted : newChain)
            if (! sameAsAny (finalTypes, wanted))
                dropped.push_back (wanted);

        for (const auto& plugin : dropped)
        {
            // Treated exactly as a departing plugin, because that is what it
            // has become. Its graph node needs no teardown here: a replacement
            // can only happen with a non-empty `arriving`, which ends this
            // function in loadActivePlugins, and that clears the graph.
            store.stageErase (plugin);
        }

        // addType returns early on a replacement and does NOT broadcast
        // (juce_KnownPluginList.cpp:111, before the sendChangeMessage at :118),
        // so an apply whose only mutation was a replacement would change the
        // list in memory and never write it. Asking for the broadcast keeps the
        // persist on its usual async path rather than adding a second writer.
        //
        // Guarded like the removeType and addType calls above, and for the same
        // reason: this is the third mutation site in this function and it was
        // the one left bare. A throw here leaves the list changed in memory with
        // a full set of staged settings uncommitted, which is precisely the
        // half-applied state the transaction exists to prevent. Nothing here is
        // expected to throw -- sendChangeMessage is an AsyncUpdater trigger and
        // allocates -- so this is what a std::bad_alloc would do, stated in the
        // same shape as its two neighbours rather than left as the odd one out.
        try
        {
            activePluginList.sendChangeMessage();
        }
        catch (const std::exception& e)
        {
            abandon ("activePluginList.sendChangeMessage", juce::String (e.what()));
            return;
        }
        catch (...)
        {
            abandon ("activePluginList.sendChangeMessage", "unknown exception");
            return;
        }

        // AFTER the broadcast above, for the same reason the departing loop
        // erases after its removeType: abandon() rolls the settings back and
        // cannot put a deleted state file back. Erasing first meant a throw
        // from the broadcast left every dropped plugin's preset gone while the
        // settings that referenced it were restored.
        for (const auto& plugin : dropped)
            statesToForget.emplace_back (ChainStore::identityOf (plugin), plugin.name);

        if (! dropped.empty())
        {
            juce::StringArray names;

            for (const auto& plugin : dropped)
                names.add (plugin.name);

            // Reported, not merely logged: the running chain is not the chain
            // that was asked for, and a silently short chain is the class of
            // failure this host has spent several releases removing.
            reportStatus ("Not added to the chain: " + names.joinIntoString (", ")
                          + ". Another plugin in the chain has the same file and"
                            " plugin id under a different format, and the plugin"
                            " list holds one entry for both.");
        }

        juce::Logger::writeToLog ("IconMenu: addType replaced an existing entry; "
                                  + juce::String ((int) dropped.size())
                                  + " plugin(s) dropped from the chain");
    }

    // ── Tear down the nodes of departed plugins ──────────────────────────────
    for (const auto& plugin : departing)
    {
        if (plugin.nodeId == 0)
            continue;

        const NodeID nodeId { static_cast<uint32> (plugin.nodeId) };
        stopListeningTo (nodeId);
        PluginWindow::closeCurrentlyOpenWindowsFor (nodeId);

        // UpdateKind::none, for the reason spelled out in handleDeletePlugin:
        // a sync removal publishes a sequence with this node's lane cut, and
        // the rewire that closes the gap does not happen until the bottom of
        // this function -- after store.commit() and flushSettings() have
        // written the settings document. Remove the last plugin in a lane with
        // Apply and that lane was silent for the length of the write.
        graph.removeNode (nodeId, juce::AudioProcessorGraph::UpdateKind::none);
    }

    // ── Write the new order, bypass and lane ─────────────────────────────────
    // Order values are plain indices. They used to be time(nullptr) + offset,
    // which is opaque, tells nothing apart from insertion time, and overflows a
    // signed 32-bit int in 2038. Every plugin in the chain is written in this one
    // pass, so the values stay consistent with each other.
    for (size_t i = 0; i < newChain.size(); ++i)
    {
        // Nothing is staged for a plugin the list dropped. Store::commit
        // applies removals before writes, so staging here would put back the
        // keys stageErase has just taken out.
        if (! dropped.empty() && sameAsAny (dropped, newChain[i]))
            continue;

        store.stageOrder (newChain[i], static_cast<int> (i));

        if (i < bypassStates.size())
            store.stageBypassed (newChain[i], bypassStates[i]);

        if (i < lanes.size())
            store.stageLane (newChain[i], lanes[i]);
    }

    store.commit();
    flushSettings (*settings, "applying the plugin chain");

    // Only now. Every abandon() path above returns before reaching this, so a
    // chain edit that was rolled back leaves every .lhs where it was -- which is
    // what the log line in the departing loop has always promised and, until
    // this was deferred, could not deliver across more than one plugin.
    for (const auto& [forgetIdentity, forgetName] : statesToForget)
        if (! forgetPluginState (vault, forgetIdentity))
            juce::Logger::writeToLog ("Could not delete saved state for " + forgetName
                                      + "; re-adding it will restore the old preset");
    sortedPluginCache.reset();

    // ── Reload the graph ─────────────────────────────────────────────────────
    // Arriving plugins need their DLLs loaded, which loadActivePlugins does for
    // the whole chain. A reorder, a bypass or a lane change needs only a rewire.
    // Arriving plugins load asynchronously, so the confirmation comes in two
    // parts: this one immediately, and "Chain updated" from onAllPluginsLoaded
    // once the DLLs are actually in. The flag keeps that second message off the
    // startup load, which goes through the same completion handler.
    applyInitiatedLoad = ! arriving.empty();

    if (preferencesWindow != nullptr)
        preferencesWindow->setApplyFeedback (arriving.empty() ? "Chain updated"
                                                             : "Loading plugins...");

    if (! arriving.empty())
    {
        loadActivePlugins();
    }
    else
    {
        reconnectGraph();

        // Push the committed chain back into the open window. Every OTHER
        // chain mutation pairs its rewire with this -- the tray bypass, move
        // and delete handlers all do -- and Apply did not, which left the
        // signal view labelled from the chain as it was before the Apply
        // while its meters came from the chain after it. That is the same
        // mis-attribution the labels were just fixed for, reached through the
        // busiest door rather than the one that was closed.
        //
        // Only on this branch. The loadActivePlugins path refreshes from
        // onAllPluginsLoaded once the instances exist, because refreshing
        // here would show rows for plugins that have not finished loading.
        refreshPreferencesIfOpen();
    }
}

//==============================================================================
#if JUCE_WINDOWS
juce::File IconMenu::getStartupShortcutPath()
{
    PWSTR path = nullptr;
    if (SUCCEEDED (SHGetKnownFolderPath (FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &path)))
    {
        const juce::File folder { juce::String (path) };
        CoTaskMemFree (path);
        return folder.getChildFile ("Light Host.lnk");
    }
    return {};
}

bool IconMenu::isStartupEnabled() const
{
    return getStartupShortcutPath().existsAsFile();
}

void IconMenu::setStartupEnabled (bool shouldEnable)
{
    const auto shortcut = getStartupShortcutPath();

    if (! shouldEnable)
    {
        shortcut.deleteFile();
        return;
    }

    IShellLinkW* psl = nullptr;
    if (FAILED (CoCreateInstance (CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**> (&psl))))
        return;

    const auto exeFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    psl->SetPath             (exeFile.getFullPathName().toWideCharPointer());
    psl->SetWorkingDirectory (exeFile.getParentDirectory().getFullPathName().toWideCharPointer());
    psl->SetDescription      (L"Light Host");

    IPersistFile* ppf = nullptr;
    if (SUCCEEDED (psl->QueryInterface (IID_IPersistFile, reinterpret_cast<void**> (&ppf))))
    {
        shortcut.getParentDirectory().createDirectory();
        ppf->Save (shortcut.getFullPathName().toWideCharPointer(), TRUE);
        ppf->Release();
    }
    psl->Release();
}
#endif

//==============================================================================
void IconMenu::reloadPlugins()
{
    if (pluginListWindow == nullptr)
        pluginListWindow = std::make_unique<PluginListWindow> (*this, formatManager);

    pluginListWindow->toFront (true);
}

