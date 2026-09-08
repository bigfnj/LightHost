#include "IconMenu.hpp"
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

        setContentOwned (new AugmentedPluginListComponent (pluginFormatManager,
                                                            owner.knownPluginList,
                                                            deadMansPedalFile,
                                                            getAppProperties().getUserSettings()),
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
                                      PropertiesFile* propsFile)
            : PluginListComponent (fmgr, kpl, deadMansFile, propsFile),
              formatManager (fmgr),
              knownList     (kpl),
              deadMansPedal (deadMansFile)
        {
            // Replace the built-in Options button onClick so we can append our item.
            getOptionsButton().onClick = [this]
            {
                auto menu = createOptionsMenu();
                menu.addSeparator();
                menu.addItem (kScanCustomFolderID, "Scan Custom Folder...");
                menu.showMenuAsync (
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
                    PluginDirectoryScanner scanner (knownList, *fmt, path, true, deadMansPedal);
                    String name;
                    while (! threadShouldExit() && scanner.scanNextFile (true, name))
                        setStatusMessage ("Scanning: " + name);
                }
            }

        private:
            AudioPluginFormatManager& formatManager;
            KnownPluginList&          knownList;
            File                      deadMansPedal;
            File                      scanDir;
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
                    job.runThread();
                });
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
        const auto savedAudioState = getAppProperties().getUserSettings()->getXmlValue ("audioDeviceState");
        const auto error = deviceManager.initialise (2, 2, savedAudioState.get(), true);

        if (error.isNotEmpty())
            status.report ("Audio device could not be opened: " + error);
    }

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);
    deviceManager.addChangeListener (this);

    logAudioConfig ("startup");

    // Known plugins
    if (auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue ("pluginList"))
        knownPluginList.recreateFromXml (*savedPluginList);

    knownPluginList.addChangeListener (this);

    // Active plugins
    if (auto savedPluginListActive = getAppProperties().getUserSettings()->getXmlValue ("pluginListActive"))
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

    // Detach the audio callback FIRST — deviceManager outlives player and graph
    // in member-destruction order (declared before them, destroyed after them).
    // Without this, deviceManager's real-time thread keeps firing
    // audioDeviceIOCallback() into player/graph while they are being destroyed,
    // which is a guaranteed use-after-free on every shutdown path (visibly
    // crashes on system restart/shutdown where the OS disrupts audio in parallel).
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);

    // A plugin may have reported a latency change moments ago. Nothing should
    // rewire the graph while it is being torn down.
    cancelPendingUpdate();

    // Abandon any in-flight load before saving. Bumping the generation makes the
    // pending createPluginInstanceAsync callbacks no-op when they arrive. Their
    // captured SafePointer already guards against this object being gone; the
    // generation bump additionally stops a late callback mutating the graph while
    // we tear down. There is no worker thread to stop, so this cannot block.
    ++pluginLoadGeneration;
    pendingLoads.clear();

    savePluginStates();
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
    static constexpr NodeID kInputNodeId  { 1'000'000u };
    static constexpr NodeID kOutputNodeId { 1'000'001u };

    juce::Logger::writeToLog ("IconMenu: loadActivePlugins begin");

    // Cancel any in-flight load. Bumping the generation makes queued async
    // callbacks from the previous run drop their results when they arrive.
    ++pluginLoadGeneration;
    pendingLoads.clear();
    nextLoadIndex = 0;
    statesNotRestored.clear();   // fresh graph — no failed restores to remember yet

    sortedCacheDirty = true;
    PluginWindow::closeAllCurrentlyOpenWindows();
    graph.clear();

    graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (
        AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode),  kInputNodeId);
    graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (
        AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode), kOutputNodeId);

    inputNodeId  = kInputNodeId;
    outputNodeId = kOutputNodeId;

    // One trim per lane, on reserved ids, created once here rather than allocated
    // like plugin nodes: there are exactly kNumLanes of them and they outlive every
    // chain edit. A lane whose trim could not be added is wired straight to the
    // output instead, so a failure here costs the trim and not the audio.
    createLaneGainNodes();

    // Wire input → output immediately so audio passes through while plugins load.
    reconnectGraph();

    const auto& sorted = getTimeSortedList();
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
            store.stageNodeId (plugin, nodeIdVal);
        }

        auto savedState = vault.read (ChainStore::identityOf (plugin));

        if (savedState.getSize() == 0)
        {
            // Falls back to the pre-5.0.0 key, so a migration that could not
            // write its file presents as a plugin that still has its preset
            // rather than one that lost it.
            const auto legacy = store.readState (plugin);

            if (legacy.isNotEmpty())
                savedState.fromBase64Encoding (legacy);
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

    const auto& spec = pendingLoads[nextLoadIndex];

    const auto sr = graph.getSampleRate() > 0.0 ? graph.getSampleRate() : 44100.0;
    const auto bs = graph.getBlockSize()  > 0   ? graph.getBlockSize()  : 512;

    const auto generation = pluginLoadGeneration;
    const auto nodeId     = spec.nodeId;
    const auto savedState = spec.savedState;
    const auto name       = spec.description.name;

    Component::SafePointer<IconMenu> safe (this);

    formatManager.createPluginInstanceAsync (
        spec.description, sr, bs,
        [safe, generation, nodeId, savedState, name]
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
            if (auto node = graph.addNode (std::move (instance), nodeId))
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
    return NodeID { 1'000'010u + static_cast<uint32> (juce::jlimit (0, lighthost::kMaxLane, lane)) };
}

void IconMenu::createLaneGainNodes()
{
    const ChainStore store (*getAppProperties().getUserSettings());

    for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
    {
        auto node = graph.addNode (std::make_unique<lighthost::gain::Processor>(),
                                   laneGainNodeId (lane));

        if (node == nullptr)
        {
            status.report ("Lane " + juce::String (lane)
                           + " trim could not be created; that lane runs at unity.");
            continue;
        }

        if (auto* processor = dynamic_cast<lighthost::gain::Processor*> (node->getProcessor()))
            processor->setGainDb (store.readLaneGainDb (lane));
    }
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

    return lighthost::state::Vault (
        settingsFile.getSiblingFile (settingsFile.getFileNameWithoutExtension() + ".state"));
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
        const auto legacy = store.readState (plugin);

        if (legacy.isEmpty())
            continue;

        MemoryBlock block;
        block.fromBase64Encoding (legacy);

        if (block.getSize() == 0)
        {
            // Undecodable. Leave it: it is the only copy there is.
            ++stayed;
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

    for (const auto& pd : getTimeSortedList())
    {
        const auto nodeIdVal = store.readNodeId (pd);
        if (nodeIdVal == 0)
            continue;

        const NodeID nodeId { static_cast<uint32> (nodeIdVal) };
        auto* node = graph.getNodeForId (nodeId);
        if (node == nullptr)
            continue;

        node->setBypassed (store.readBypassed (pd));

        auto* processor = node->getProcessor();
        if (processor == nullptr)
            continue;

        // Lane comes straight from the settings file, so it is clamped rather
        // than trusted; nothing else validates it.
        lighthost::topology::NodeFacts facts;
        facts.nodeId            = nodeId;
        facts.lane              = store.readLane (pd);
        facts.numInputChannels  = processor->getTotalNumInputChannels();
        facts.numOutputChannels = processor->getTotalNumOutputChannels();

        if (! lighthost::topology::canPassAudio (facts))
            juce::Logger::writeToLog ("IconMenu: " + pd.name + " has "
                                      + juce::String (facts.numInputChannels) + " in / "
                                      + juce::String (facts.numOutputChannels)
                                      + " out channels, so it cannot sit in a lane; wiring around it");

        layout.nodes.push_back (facts);
    }

    const auto desired = lighthost::topology::buildConnections (layout);
    const auto diff    = lighthost::topology::applyConnections (graph, desired);

    // One publish to the audio thread, whatever changed.
    graph.rebuild();

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

    const auto decision = lighthost::samplerate::decide (setup.sampleRate,
                                                         device->getAvailableSampleRates(),
                                                         sampleRateCorrections);

    if (decision.resetAttempts)
        sampleRateCorrections = 0;

    switch (decision.action)
    {
        case lighthost::samplerate::Action::keepCurrentRate:
            return;

        case lighthost::samplerate::Action::giveUp:
            // Logged once per episode: the counter only advances on a request, so
            // this cannot repeat until the device settles and goes wrong again.
            ++sampleRateCorrections;
            juce::Logger::writeToLog ("IconMenu: giving up on the sample rate after "
                                      + juce::String (lighthost::samplerate::kMaxCorrections)
                                      + " attempts. The driver keeps reporting "
                                      + juce::String (setup.sampleRate, 0)
                                      + "Hz, which it also says it does not support.");
            return;

        case lighthost::samplerate::Action::applyRate:
            ++sampleRateCorrections;
            juce::Logger::writeToLog ("IconMenu: sample rate " + juce::String (setup.sampleRate, 0)
                                      + "Hz is unsupported; asking for "
                                      + juce::String (decision.rate, 0) + "Hz (attempt "
                                      + juce::String (sampleRateCorrections) + ")");
            setup.sampleRate = decision.rate;
            deviceManager.setAudioDeviceSetup (setup, true);
            return;
    }
}

//==============================================================================
const std::vector<PluginDescription>& IconMenu::getTimeSortedList() const
{
    if (! sortedCacheDirty)
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

    sortedPluginCache = ChainStore::sortByOrder (std::move (orderedTypes));
    sortedCacheDirty  = false;
    return sortedPluginCache;
}

//==============================================================================
void IconMenu::changeListenerCallback (ChangeBroadcaster* changed)
{
    auto* settings = getAppProperties().getUserSettings();

    if (changed == &knownPluginList)
    {
        if (auto xml = knownPluginList.createXml())
        {
            settings->setValue ("pluginList", xml.get());
            flushSettings (*settings, "saving the scanned plugin list");
        }
    }
    else if (changed == &activePluginList)
    {
        if (auto xml = activePluginList.createXml())
        {
            settings->setValue ("pluginListActive", xml.get());
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

        if (auto xml = deviceManager.createStateXml())
        {
            settings->setValue ("audioDeviceState", xml.get());
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

    const auto& timeSorted = getTimeSortedList();
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

    #if JUCE_MAC || JUCE_LINUX
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

    // Plugin actions
    if (id >= kDeleteOffset && id < kDeleteOffset + kEditOffset)
        im->handleDeletePlugin (id - kDeleteOffset);
    else if (id >= kMoveDownOffset && id < kMoveDownOffset + kEditOffset)
        im->handleMovePlugin (id - kMoveDownOffset, false);
    else if (id >= kMoveUpOffset && id < kMoveUpOffset + kEditOffset)
        im->handleMovePlugin (id - kMoveUpOffset, true);
    else if (id >= kBypassOffset && id < kBypassOffset + kEditOffset)
        im->handleBypassPlugin (id - kBypassOffset);
    else if (id >= kEditOffset && id < kEditOffset + kEditOffset)
        im->handleEditPlugin (id - kEditOffset);

    im->startTimer (50);
}

//==============================================================================
void IconMenu::handleDeletePlugin (int index)
{
    const auto& timeSorted = getTimeSortedList();
    if (index < 0 || index >= static_cast<int> (timeSorted.size()))
        return;

    // Copied, not referenced. getTimeSortedList hands out a reference to a cache
    // that the next rebuild clears, and this function outlives that rebuild.
    const auto pluginToDelete = timeSorted[static_cast<size_t> (index)];
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

    // Remove from the list first. It is the mutation that can throw (its change
    // listener writes the XML), and nothing destructive should happen until it
    // has succeeded.
    try
    {
        activePluginList.removeType (pluginToDelete);  // triggers changeListener → persists XML
    }
    catch (const std::exception& e)
    {
        store.rollback();
        juce::Logger::writeToLog ("activePluginList.removeType threw std::exception for "
                                  + pluginToDelete.name + ": " + juce::String (e.what())
                                  + "; the plugin's settings were left untouched");
        refreshPreferencesIfOpen();
        return;
    }
    catch (...)
    {
        store.rollback();
        juce::Logger::writeToLog ("activePluginList.removeType threw unknown exception for "
                                  + pluginToDelete.name
                                  + "; the plugin's settings were left untouched");
        refreshPreferencesIfOpen();
        return;
    }

    if (nodeIdVal != 0)
    {
        const NodeID nodeId { static_cast<uint32> (nodeIdVal) };
        stopListeningTo (nodeId);
        PluginWindow::closeCurrentlyOpenWindowsFor (nodeId);  // close UI first
        graph.removeNode (nodeId);                             // destroy only this instance
    }

    store.commit();
    flushSettings (*settings, "deleting a plugin");

    sortedCacheDirty = true;
    reconnectGraph();  // rewire around the removed node — no plugin loads needed
    refreshPreferencesIfOpen();
}

void IconMenu::handleBypassPlugin (int index)
{
    const auto& timeSorted = getTimeSortedList();
    if (index < 0 || index >= static_cast<int> (timeSorted.size()))
        return;

    const auto plugin = timeSorted[static_cast<size_t> (index)];   // copied: see handleDeletePlugin

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
    const auto& sorted = getTimeSortedList();
    if (index < 0 || index >= static_cast<int> (sorted.size()))
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
    const auto& timeSorted  = getTimeSortedList();
    if (index < 0 || index >= static_cast<int> (timeSorted.size()))
        return;

    const int neighborIndex = moveUp ? index - 1 : index + 1;

    if (neighborIndex < 0 || neighborIndex >= static_cast<int> (timeSorted.size()))
        return;

    const auto target   = timeSorted[static_cast<size_t> (index)];          // copied: see
    const auto neighbor = timeSorted[static_cast<size_t> (neighborIndex)];  // handleDeletePlugin

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

    sortedCacheDirty = true;
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
    const auto list = getTimeSortedList();
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

    // Button ORDER is chosen per platform, and the index to act on is derived
    // from that order rather than hard-coded, because the platforms disagree
    // about what a dismissal reports.
    //
    //   Windows: button ids are 0..N-1 in order, Escape is disabled (no
    //            TDF_ALLOW_DIALOG_CANCELLATION), and a TaskDialogIndirect
    //            failure leaves the result at 0. So the destructive button
    //            must not be index 0.
    //   Linux:   the raw AlertWindow result is remapped (raw + N - 1) % N, and
    //            the LookAndFeel binds Escape to the *second* button, while
    //            userTriedToCloseWindow() exits with raw 0. Both resolve to the
    //            LAST index, so the destructive button must not be last.
    //
    // No single order is safe on both, which is why this is not just
    // "Cancel first" with a comment claiming Escape is harmless.
   #if JUCE_LINUX || JUCE_BSD
    const juce::StringArray order { "Delete", "Cancel" };
   #else
    const juce::StringArray order { "Cancel", "Delete" };
   #endif

    const auto deleteIndex = order.indexOf ("Delete");

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

            safe->deletePluginStates();
            safe->loadActivePlugins();
            safe->reportStatus ("Deleted saved plugin states");
        });
}

void IconMenu::deletePluginStates()
{
    const auto list = getTimeSortedList();
    auto* settings = getAppProperties().getUserSettings();
    ChainStore store (*settings);

    const auto vault = stateVault();

    for (const auto& plugin : list)
    {
        store.stageState (plugin, {});   // clears any un-migrated legacy blob
        (void) vault.erase (ChainStore::identityOf (plugin));
        lastWrittenState.erase (ChainStore::identityOf (plugin));
    }

    store.commit();
    flushSettings (*settings, "clearing saved plugin states");
}

void IconMenu::savePluginStates()
{
    const auto& list = getTimeSortedList();
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
    const auto chain = getTimeSortedList();
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
        [safe]()
        {
            if (auto* im = safe.getComponent())
            {
                juce::Logger::writeToLog ("IconMenu: closing Preferences window");

                // Persist audio device state when the window is closed
                auto audioState = im->deviceManager.createStateXml();
                if (audioState != nullptr)
                {
                    auto* settings = getAppProperties().getUserSettings();
                    settings->setValue ("audioDeviceState", audioState.get());
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

    const auto chain = getTimeSortedList();
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

    const auto& currentChain = getTimeSortedList();

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
    const auto abandon = [&] (const juce::String& what, const juce::String& detail)
    {
        store.rollback();
        juce::Logger::writeToLog ("IconMenu: " + what + " threw (" + detail
                                  + "); chain edit abandoned, settings untouched");
        sortedCacheDirty = true;
        reconnectGraph();
        refreshPreferencesIfOpen();
    };

    const auto vault = stateVault();

    for (const auto& plugin : departing)
    {
        store.stageErase (plugin.description);

        // The state file goes with the settings keys, or the directory
        // accumulates orphans for plugins that are no longer in the chain.
        const auto departingIdentity = ChainStore::identityOf (plugin.description);
        (void) vault.erase (departingIdentity);
        lastWrittenState.erase (departingIdentity);

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
    }

    for (const auto& plugin : arriving)
    {
        if (store.readNodeId (plugin) == 0)
            store.stageNodeId (plugin, store.allocateNodeId());

        try
        {
            activePluginList.addType (plugin);  // changeListener persists the XML
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

    // ── Tear down the nodes of departed plugins ──────────────────────────────
    for (const auto& plugin : departing)
    {
        if (plugin.nodeId == 0)
            continue;

        const NodeID nodeId { static_cast<uint32> (plugin.nodeId) };
        stopListeningTo (nodeId);
        PluginWindow::closeCurrentlyOpenWindowsFor (nodeId);
        graph.removeNode (nodeId);
    }

    // ── Write the new order, bypass and lane ─────────────────────────────────
    // Order values are plain indices. They used to be time(nullptr) + offset,
    // which is opaque, tells nothing apart from insertion time, and overflows a
    // signed 32-bit int in 2038. Every plugin in the chain is written in this one
    // pass, so the values stay consistent with each other.
    for (size_t i = 0; i < newChain.size(); ++i)
    {
        store.stageOrder (newChain[i], static_cast<int> (i));

        if (i < bypassStates.size())
            store.stageBypassed (newChain[i], bypassStates[i]);

        if (i < lanes.size())
            store.stageLane (newChain[i], lanes[i]);
    }

    store.commit();
    flushSettings (*settings, "applying the plugin chain");
    sortedCacheDirty = true;

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
        loadActivePlugins();
    else
        reconnectGraph();
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

