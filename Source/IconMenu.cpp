#include "IconMenu.hpp"
#include "PluginWindow.h"
#include "PreferencesWindow.h"
#include <BinaryData.h>
#include <algorithm>
#include <ctime>
#include <exception>

#if JUCE_WINDOWS
#include <windows.h>
#include <shlobj.h>
#endif

using namespace juce;

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
        setResizeLimits (300, 400, 800, 1500);
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
        owner.removePluginsLackingInputOutput();

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
        }

    private:
        static constexpr int kScanCustomFolderID = 9999;

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
// Loads plugin instances on a background thread one at a time, then marshals
// each completed instance back to the message thread via callAsync so the UI
// stays responsive during startup and full reloads.
//
// Edge-case behaviour: if a plugin is deleted from the active list while this
// thread is running, onPluginInstanceReady checks activePluginList and discards
// the stale instance rather than adding an orphaned node.
class IconMenu::PluginLoadThread final : public juce::Thread
{
public:
    struct LoadSpec
    {
        juce::PluginDescription description;
        juce::String            savedState;
        juce::uint32            nodeId;
    };

    PluginLoadThread (IconMenu& ownerRef,
                      std::vector<LoadSpec> specsToLoad,
                      juce::AudioPluginFormatManager& formatManagerToUse,
                      double sampleRateToUse,
                      int blockSizeToUse,
                      int generationToUse)
        : juce::Thread ("LightHost Plugin Loader"),
          owner         (ownerRef),
          specs         (std::move (specsToLoad)),
          formatManager (formatManagerToUse),
          sampleRate    (sampleRateToUse),
          blockSize     (blockSizeToUse),
          generation    (generationToUse)
    {}

    void run() override
    {
        for (auto& spec : specs)
        {
            if (threadShouldExit())
                return;

            juce::String error;
            auto instance = formatManager.createPluginInstance (
                spec.description, sampleRate, blockSize, error);

            if (instance == nullptr)
            {
                DBG ("PluginLoadThread: failed to load " + spec.description.name + " - " + error);
                juce::Logger::writeToLog ("Plugin load failed: " + spec.description.name + " - " + error);
                continue;
            }

            if (threadShouldExit())
                return;

            // Transfer ownership to the message thread.
            // SafePointer ensures the lambda is a no-op if IconMenu is
            // destroyed before it fires; the unique_ptr still cleans up.
            auto* raw  = instance.release();
            auto  nid  = spec.nodeId;
            auto  gen  = generation;
            auto  state = spec.savedState;
            juce::Component::SafePointer<IconMenu> safe (&owner);

            juce::MessageManager::callAsync ([safe, raw, nid, gen, state]() mutable
            {
                std::unique_ptr<juce::AudioProcessor> inst (raw);
                if (auto* im = safe.getComponent())
                    im->onPluginInstanceReady (std::move (inst),
                                               juce::AudioProcessorGraph::NodeID { nid },
                                               gen,
                                               std::move (state));
                // If safe is null (IconMenu destroyed), inst is deleted here — no leak.
            });
        }

        // All specs dispatched — signal completion.
        auto gen = generation;
        juce::Component::SafePointer<IconMenu> safe (&owner);
        juce::MessageManager::callAsync ([safe, gen]()
        {
            if (auto* im = safe.getComponent())
                im->onAllPluginsLoaded (gen);
        });
    }

private:
    IconMenu&                       owner;
    std::vector<LoadSpec>           specs;
    juce::AudioPluginFormatManager& formatManager;
    double                          sampleRate;
    int                             blockSize;
    int                             generation;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginLoadThread)
};

//==============================================================================
IconMenu::IconMenu()
{
    juce::Logger::writeToLog ("IconMenu: constructing");

    // JUCE 8: addDefaultFormats() removed from AudioPluginFormatManager.
    // Use the free function addDefaultFormatsToManager() instead.
    addDefaultFormatsToManager (formatManager);

    // Audio device initialization
    if (auto savedAudioState = getAppProperties().getUserSettings()->getXmlValue ("audioDeviceState"))
        deviceManager.initialise (2, 2, savedAudioState.get(), true);
    else
        deviceManager.initialise (2, 2, nullptr, true);

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);
    deviceManager.addChangeListener (this);

    // Known plugins
    if (auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue ("pluginList"))
        knownPluginList.recreateFromXml (*savedPluginList);

    knownPluginList.addChangeListener (this);

    // Active plugins
    if (auto savedPluginListActive = getAppProperties().getUserSettings()->getXmlValue ("pluginListActive"))
        activePluginList.recreateFromXml (*savedPluginListActive);

    loadActivePlugins();
    activePluginList.addChangeListener (this);

    setIcon();
    setIconTooltip (JUCEApplication::getInstance()->getApplicationName());
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

    // Stop the background load thread before saving states.
    // This ensures no concurrent graph mutations and that no pending
    // callAsync lambdas reference this object after it starts tearing down.
    if (pluginLoadThread != nullptr)
    {
        pluginLoadThread->stopThread (3000);
        pluginLoadThread.reset();
    }

    savePluginStates();
}

//==============================================================================
void IconMenu::cancelPluginLoading()
{
    if (pluginLoadThread == nullptr)
        return;

    ++pluginLoadGeneration;
    pluginLoadThread->stopThread (3000);
    pluginLoadThread.reset();
    setIconTooltip (JUCEApplication::getInstance()->getApplicationName());
    juce::Logger::writeToLog ("IconMenu: cancelled in-flight plugin load");
}

//==============================================================================
void IconMenu::setIcon()
{
    const auto icon = ImageFileFormat::loadFrom (BinaryData::MicrophoneIcon_png, BinaryData::MicrophoneIcon_pngSize);
    setIconImage (icon, icon);
}

//==============================================================================
void IconMenu::loadActivePlugins()
{
    static constexpr NodeID kInputNodeId  { 1'000'000u };
    static constexpr NodeID kOutputNodeId { 1'000'001u };

    juce::Logger::writeToLog ("IconMenu: loadActivePlugins begin");

    // Cancel any in-flight load.  Incrementing the generation causes any
    // callAsync lambdas already queued from the previous run to be discarded
    // when they arrive on the message thread.
    ++pluginLoadGeneration;
    if (pluginLoadThread != nullptr)
    {
        pluginLoadThread->stopThread (3000);
        pluginLoadThread.reset();
    }

    sortedCacheDirty = true;
    PluginWindow::closeAllCurrentlyOpenWindows();
    graph.clear();

    graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (
        AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode),  kInputNodeId);
    graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (
        AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode), kOutputNodeId);

    inputNodeId  = kInputNodeId;
    outputNodeId = kOutputNodeId;

    // Wire input → output immediately so audio passes through while plugins load.
    reconnectGraph();

    const auto& sorted = getTimeSortedList();
    if (sorted.empty())
        return;

    // Assign NodeIDs on the message thread before handing off — settings
    // writes must not happen from the background thread.
    auto* settings   = getAppProperties().getUserSettings();
    const auto sr    = graph.getSampleRate() > 0.0 ? graph.getSampleRate() : 44100.0;
    const auto bs    = graph.getBlockSize()  > 0   ? graph.getBlockSize()  : 512;

    std::vector<PluginLoadThread::LoadSpec> specs;
    specs.reserve (sorted.size());

    for (const auto& plugin : sorted)
    {
        auto nodeIdVal = settings->getIntValue (getKey ("nodeid", plugin), 0);
        if (nodeIdVal == 0)
        {
            nodeIdVal = settings->getIntValue ("nextPluginNodeId", 1);
            settings->setValue ("nextPluginNodeId", nodeIdVal + 1);
            settings->setValue (getKey ("nodeid", plugin), nodeIdVal);
        }
        specs.push_back ({ plugin,
                           settings->getValue (getKey ("state", plugin)),
                           static_cast<uint32> (nodeIdVal) });
    }
    settings->saveIfNeeded();

    setIconTooltip ("Light Host - loading...");

    pluginLoadThread = std::make_unique<PluginLoadThread> (
        *this, std::move (specs), formatManager, sr, bs, pluginLoadGeneration);
    pluginLoadThread->startThread();
}

//==============================================================================
void IconMenu::onPluginInstanceReady (std::unique_ptr<AudioProcessor> instance,
                                      AudioProcessorGraph::NodeID nodeId,
                                      int generation,
                                      juce::String savedState)
{
    if (generation != pluginLoadGeneration)
        return;  // stale result — a newer loadActivePlugins() call cancelled this load

    if (instance == nullptr)
        return;

    if (savedState.isNotEmpty())
    {
        try
        {
            juce::MemoryBlock block;
            block.fromBase64Encoding (savedState);
            instance->setStateInformation (block.getData(),
                                           static_cast<int> (block.getSize()));
        }
        catch (const std::exception& e)
        {
            juce::Logger::writeToLog ("Plugin state restore threw std::exception for node "
                                      + juce::String ((int) nodeId.uid) + ": " + e.what());
        }
        catch (...)
        {
            juce::Logger::writeToLog ("Plugin state restore threw unknown exception for node "
                                      + juce::String ((int) nodeId.uid));
        }
    }

    // Guard against the plugin having been deleted from the active list while
    // the background thread was loading it.  If the NodeID is no longer in
    // settings the plugin is unwanted; the unique_ptr destructor cleans up.
    auto* settings = getAppProperties().getUserSettings();
    for (const auto& plugin : activePluginList.getTypes())
    {
        if (static_cast<uint32> (settings->getIntValue (getKey ("nodeid", plugin), 0))
                == nodeId.uid)
        {
            graph.addNode (std::move (instance), nodeId);
            juce::Logger::writeToLog ("IconMenu: plugin instance ready for " + plugin.name);
            return;
        }
    }
}

void IconMenu::onAllPluginsLoaded (int generation)
{
    if (generation != pluginLoadGeneration)
        return;

    pluginLoadThread.reset();
    reconnectGraph();
    setIconTooltip (JUCEApplication::getInstance()->getApplicationName());
    juce::Logger::writeToLog ("IconMenu: loadActivePlugins complete");
}

//==============================================================================
void IconMenu::reconnectGraph()
{
    static constexpr int kChannelOne = 0;
    static constexpr int kChannelTwo = 1;

    for (auto& c : graph.getConnections())
        graph.removeConnection (c);

    const auto& sorted = getTimeSortedList();
    const auto numPlugins = static_cast<int> (sorted.size());

    if (numPlugins == 0)
    {
        graph.addConnection ({ { inputNodeId, kChannelOne }, { outputNodeId, kChannelOne } });
        if (! graph.addConnection ({ { inputNodeId, kChannelTwo }, { outputNodeId, kChannelTwo } }))
            graph.addConnection ({ { inputNodeId, kChannelOne }, { outputNodeId, kChannelTwo } });
        return;
    }

    auto* settings = getAppProperties().getUserSettings();
    NodeID lastActiveNodeId {};
    bool hasInputConnected = false;

    for (int i = 0; i < numPlugins; ++i)
    {
        const auto& plugin = sorted[static_cast<size_t> (i)];
        const auto nodeIdVal = settings->getIntValue (getKey ("nodeid", plugin), 0);
        if (nodeIdVal == 0)
            continue;

        const NodeID nodeId { static_cast<uint32> (nodeIdVal) };
        if (graph.getNodeForId (nodeId) == nullptr)
            continue;

        if (settings->getBoolValue (getKey ("bypass", plugin), false))
            continue;

        if (! hasInputConnected)
        {
            graph.addConnection ({ { inputNodeId, kChannelOne }, { nodeId, kChannelOne } });
            if (! graph.addConnection ({ { inputNodeId, kChannelTwo }, { nodeId, kChannelTwo } }))
                graph.addConnection ({ { inputNodeId, kChannelOne }, { nodeId, kChannelTwo } });
            hasInputConnected = true;
        }
        else
        {
            graph.addConnection ({ { lastActiveNodeId, kChannelOne }, { nodeId, kChannelOne } });
            graph.addConnection ({ { lastActiveNodeId, kChannelTwo }, { nodeId, kChannelTwo } });
        }

        lastActiveNodeId = nodeId;
    }

    if (lastActiveNodeId.uid != 0)
    {
        graph.addConnection ({ { lastActiveNodeId, kChannelOne }, { outputNodeId, kChannelOne } });
        graph.addConnection ({ { lastActiveNodeId, kChannelTwo }, { outputNodeId, kChannelTwo } });
    }
    else
    {
        // All plugins bypassed — wire input directly to output
        graph.addConnection ({ { inputNodeId, kChannelOne }, { outputNodeId, kChannelOne } });
        if (! graph.addConnection ({ { inputNodeId, kChannelTwo }, { outputNodeId, kChannelTwo } }))
            graph.addConnection ({ { inputNodeId, kChannelOne }, { outputNodeId, kChannelTwo } });
    }
}

//==============================================================================
void IconMenu::autoMatchSampleRate()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    auto setup = deviceManager.getAudioDeviceSetup();
    const auto available = device->getAvailableSampleRates();

    // If the current rate is already supported, nothing to do.
    if (available.contains (setup.sampleRate))
        return;

    // Priority list: prefer studio-standard rates, fall back to whatever the device offers.
    static constexpr double kPreferred[] = { 48000.0, 44100.0, 96000.0, 88200.0, 192000.0, 32000.0 };
    for (double rate : kPreferred)
    {
        if (available.contains (rate))
        {
            setup.sampleRate = rate;
            deviceManager.setAudioDeviceSetup (setup, true);
            return;
        }
    }

    // Last resort: take whatever the device reports as its highest rate.
    if (! available.isEmpty())
    {
        setup.sampleRate = available.getLast();
        deviceManager.setAudioDeviceSetup (setup, true);
    }
}

//==============================================================================
const std::vector<PluginDescription>& IconMenu::getTimeSortedList() const
{
    if (! sortedCacheDirty)
        return sortedPluginCache;

    auto* settings = getAppProperties().getUserSettings();
    const auto types = activePluginList.getTypes();

    // Pre-read every order value once — O(N) settings reads — so the sort
    // comparator uses O(1) integer comparisons instead of reading settings
    // and allocating strings on every comparison call (was O(N log N) reads).
    std::vector<std::pair<int, PluginDescription>> orderedTypes;
    orderedTypes.reserve (static_cast<size_t> (types.size()));
    for (int i = 0; i < types.size(); ++i)
        orderedTypes.emplace_back (settings->getIntValue (getKey ("order", types[i]), 0), types[i]);

    std::sort (orderedTypes.begin(), orderedTypes.end(),
               [] (const auto& a, const auto& b) { return a.first < b.first; });

    sortedPluginCache.clear();
    sortedPluginCache.reserve (orderedTypes.size());
    for (auto& [order, pd] : orderedTypes)
        sortedPluginCache.push_back (std::move (pd));

    sortedCacheDirty = false;
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
            settings->saveIfNeeded();
        }
    }
    else if (changed == &activePluginList)
    {
        if (auto xml = activePluginList.createXml())
        {
            settings->setValue ("pluginListActive", xml.get());
            settings->saveIfNeeded();
        }
    }
    else if (changed == &deviceManager)
    {
        if (isHandlingDeviceChange)
            return;

        // ScopedValueSetter ensures the flag is cleared even if an exception
        // is thrown — prevents permanently silencing future device-change callbacks.
        const juce::ScopedValueSetter<bool> guard (isHandlingDeviceChange, true, false);

        reconnectGraph();
        autoMatchSampleRate();
        juce::Logger::writeToLog ("IconMenu: audio device change handled for type "
                                  + deviceManager.getCurrentAudioDeviceType());

        if (auto xml = deviceManager.createStateXml())
        {
            settings->setValue ("audioDeviceState", xml.get());
            settings->saveIfNeeded();
        }
    }
}

//==============================================================================
String IconMenu::getKey (const String& type, const PluginDescription& plugin)
{
    return "plugin-" + type.toLowerCase() + "-" + plugin.name + plugin.version + plugin.pluginFormatName;
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
    auto* settings = getAppProperties().getUserSettings();

    for (size_t i = 0; i < timeSorted.size(); ++i)
    {
        PopupMenu options;
        options.addItem (kEditOffset + static_cast<int> (i), "Edit");

        const auto key = getKey ("bypass", timeSorted[i]);
        const auto bypass = settings->getBoolValue (key);
        options.addItem (kBypassOffset + static_cast<int> (i), "Bypass", true, bypass);
        options.addSeparator();
        options.addItem (kMoveUpOffset + static_cast<int> (i), "Move Up", i > 0);
        options.addItem (kMoveDownOffset + static_cast<int> (i), "Move Down", i < timeSorted.size() - 1);
        options.addSeparator();
        options.addItem (kDeleteOffset + static_cast<int> (i), "Delete");

        menu.addSubMenu (timeSorted[i].name, options);
    }

    menu.addSeparator();
    #if JUCE_WINDOWS
    menu.addItem (5, "Run at Startup", true, isStartupEnabled());
    menu.addSeparator();
    #endif
    menu.addItem (3, "Quit");
    menu.addItem (4, "Delete Plugin States");

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
    if (id == 4) { im->deletePluginStates(); im->loadActivePlugins(); return; }
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

    const auto& pluginToDelete = timeSorted[static_cast<size_t> (index)];
    juce::Logger::writeToLog ("IconMenu: deleting plugin " + pluginToDelete.name);

    cancelPluginLoading();

    // Save surviving plugin states before we discard the deleted plugin's node.
    savePluginStates();

    auto* settings = getAppProperties().getUserSettings();
    const auto nodeIdVal = settings->getIntValue (getKey ("nodeid", pluginToDelete), 0);

    if (nodeIdVal != 0)
    {
        const NodeID nodeId { static_cast<uint32> (nodeIdVal) };
        PluginWindow::closeCurrentlyOpenWindowsFor (nodeId);  // close UI first
        graph.removeNode (nodeId);                             // destroy only this instance
    }

    settings->removeValue (getKey ("order",  pluginToDelete));
    settings->removeValue (getKey ("bypass", pluginToDelete));
    settings->removeValue (getKey ("state",  pluginToDelete));
    settings->removeValue (getKey ("nodeid", pluginToDelete));
    settings->saveIfNeeded();

    activePluginList.removeType (pluginToDelete);  // triggers changeListener → persists XML
    sortedCacheDirty = true;
    reconnectGraph();  // rewire around the removed node — no plugin loads needed
    refreshPreferencesIfOpen();
}

void IconMenu::handleBypassPlugin (int index)
{
    const auto& timeSorted = getTimeSortedList();
    if (index < 0 || index >= static_cast<int> (timeSorted.size()))
        return;

    const auto key = getKey ("bypass", timeSorted[static_cast<size_t> (index)]);

    auto* settings = getAppProperties().getUserSettings();
    settings->setValue (key, ! settings->getBoolValue (key));
    settings->saveIfNeeded();

    reconnectGraph();
    refreshPreferencesIfOpen();
}

void IconMenu::handleEditPlugin (int index)
{
    const auto& sorted = getTimeSortedList();
    if (index < 0 || index >= static_cast<int> (sorted.size()))
        return;

    const auto nodeIdVal = getAppProperties().getUserSettings()->getIntValue (getKey ("nodeid", sorted[static_cast<size_t> (index)]), 0);
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

    const auto& target   = timeSorted[static_cast<size_t> (index)];
    const auto& neighbor = timeSorted[static_cast<size_t> (neighborIndex)];

    // Swap only the two affected order values — O(1) writes and 4 key
    // allocations.  Previously rewrote all N values and called
    // savePluginStates() even though plugin state is unchanged by a move.
    auto* settings = getAppProperties().getUserSettings();
    const auto targetOrder   = settings->getIntValue (getKey ("order", target),   0);
    const auto neighborOrder = settings->getIntValue (getKey ("order", neighbor), 0);

    settings->setValue (getKey ("order", target),   neighborOrder);
    settings->setValue (getKey ("order", neighbor), targetOrder);
    settings->saveIfNeeded();

    sortedCacheDirty = true;
    reconnectGraph();
    refreshPreferencesIfOpen();
}

//==============================================================================
void IconMenu::deletePluginStates()
{
    const auto list = getTimeSortedList();
    auto* settings = getAppProperties().getUserSettings();

    for (const auto& plugin : list)
        settings->removeValue (getKey ("state", plugin));

    settings->saveIfNeeded();
}

void IconMenu::savePluginStates()
{
    const auto& list = getTimeSortedList();
    auto* settings = getAppProperties().getUserSettings();

    for (const auto& plugin : list)
    {
        const auto nodeIdVal = settings->getIntValue (getKey ("nodeid", plugin), 0);
        if (nodeIdVal == 0)
            continue;

        auto* node = graph.getNodeForId (NodeID { static_cast<uint32> (nodeIdVal) });
        if (node == nullptr)
            continue;

        try
        {
            MemoryBlock savedStateBinary;
            node->getProcessor()->getStateInformation (savedStateBinary);
            settings->setValue (getKey ("state", plugin), savedStateBinary.toBase64Encoding());
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

    settings->saveIfNeeded();
}

//==============================================================================
void IconMenu::showPreferences()
{
    if (preferencesWindow != nullptr)
    {
        preferencesWindow->toFront (true);
        return;
    }

    juce::Logger::writeToLog ("IconMenu: opening Preferences window");

    Component::SafePointer<IconMenu> safe (this);

    // Build initial bypass states from persisted settings
    const auto chain = getTimeSortedList();
    std::vector<bool> bypassStates;
    bypassStates.reserve (chain.size());
    auto* settings = getAppProperties().getUserSettings();
    for (const auto& plugin : chain)
        bypassStates.push_back (settings->getBoolValue (getKey ("bypass", plugin), false));

    preferencesWindow = std::make_unique<PreferencesWindow> (
        deviceManager,
        knownPluginList,
        chain,
        bypassStates,
        [safe] (const std::vector<PluginDescription>& newChain,
                const std::vector<bool>& newBypass)
        {
            if (auto* im = safe.getComponent())
                im->applyPluginChain (newChain, newBypass);
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
                    getAppProperties().getUserSettings()->setValue ("audioDeviceState",
                                                                     audioState.get());
                    getAppProperties().getUserSettings()->saveIfNeeded();
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
}

void IconMenu::refreshPreferencesIfOpen()
{
    if (preferencesWindow == nullptr) return;

    const auto chain = getTimeSortedList();
    auto* settings   = getAppProperties().getUserSettings();
    std::vector<bool> bypass;
    bypass.reserve (chain.size());
    for (const auto& p : chain)
        bypass.push_back (settings->getBoolValue (getKey ("bypass", p), false));

    preferencesWindow->refreshPluginChain (chain, bypass);
}

void IconMenu::openPluginEditorFor (const PluginDescription& pd)
{
    const auto nodeIdVal = getAppProperties().getUserSettings()
                               ->getIntValue (getKey ("nodeid", pd), 0);
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
                                  const std::vector<bool>& bypassStates)
{
    // Helper: stable string identity for matching plugins across lists.
    auto identity = [] (const PluginDescription& pd) -> String
    {
        return pd.fileOrIdentifier + pd.pluginFormatName + pd.name;
    };

    auto* settings = getAppProperties().getUserSettings();
    const auto& currentChain = getTimeSortedList();

    bool identicalOrderAndBypass = currentChain.size() == newChain.size();
    for (size_t i = 0; identicalOrderAndBypass && i < newChain.size(); ++i)
    {
        if (identity (currentChain[i]) != identity (newChain[i]))
        {
            identicalOrderAndBypass = false;
            break;
        }

        const bool currentBypass = settings->getBoolValue (getKey ("bypass", currentChain[i]), false);
        const bool requestedBypass = i < bypassStates.size() ? bypassStates[i] : false;
        if (currentBypass != requestedBypass)
            identicalOrderAndBypass = false;
    }

    if (identicalOrderAndBypass)
    {
        juce::Logger::writeToLog ("IconMenu: Apply pressed with no plugin-chain changes");
        return;
    }

    // Snapshot the current active list before mutations so iterators stay valid.
    const auto currentTypes = activePluginList.getTypes();

    bool hasStructuralChanges = currentTypes.size() != static_cast<int> (newChain.size());
    for (int i = 0; ! hasStructuralChanges && i < currentTypes.size(); ++i)
    {
        const auto& existing = currentTypes[i];
        const bool inNew = std::any_of (newChain.begin(), newChain.end(),
                                        [&] (const PluginDescription& np)
                                        { return identity (np) == identity (existing); });
        if (! inNew)
            hasStructuralChanges = true;
    }

    if (hasStructuralChanges)
        cancelPluginLoading();

    juce::Logger::writeToLog ("IconMenu: applying plugin chain (" + juce::String ((int) currentChain.size())
                              + " -> " + juce::String ((int) newChain.size()) + " plugins)");

    // Persist current plugin states before making any graph changes.
    savePluginStates();

    // ── Step 1: remove plugins absent from the new chain ────────────────────
    for (int i = 0; i < currentTypes.size(); ++i)
    {
        const auto& existing = currentTypes[i];
        const bool inNew = std::any_of (newChain.begin(), newChain.end(),
                                         [&] (const PluginDescription& np)
                                         { return identity (np) == identity (existing); });
        if (inNew) continue;

        const auto nodeIdVal = settings->getIntValue (getKey ("nodeid", existing), 0);
        if (nodeIdVal != 0)
        {
            const NodeID nodeId { static_cast<uint32> (nodeIdVal) };
            PluginWindow::closeCurrentlyOpenWindowsFor (nodeId);
            graph.removeNode (nodeId);
        }

        settings->removeValue (getKey ("order",  existing));
        settings->removeValue (getKey ("bypass", existing));
        settings->removeValue (getKey ("state",  existing));
        settings->removeValue (getKey ("nodeid", existing));
        activePluginList.removeType (existing);  // triggers changeListener → persists XML
    }

    // ── Step 2: assign NodeIDs and register any newly added plugins ──────────
    bool hasNewPlugins = false;
    for (const auto& np : newChain)
    {
        const bool inCurrent = std::any_of (currentTypes.begin(), currentTypes.end(),
                                             [&] (const PluginDescription& ep)
                                             { return identity (ep) == identity (np); });
        if (inCurrent) continue;

        auto nodeIdVal = settings->getIntValue (getKey ("nodeid", np), 0);
        if (nodeIdVal == 0)
        {
            nodeIdVal = settings->getIntValue ("nextPluginNodeId", 1);
            settings->setValue ("nextPluginNodeId", nodeIdVal + 1);
            settings->setValue (getKey ("nodeid", np), nodeIdVal);
        }
        activePluginList.addType (np);  // triggers changeListener → persists XML
        hasNewPlugins = true;
    }

    // ── Step 3: write new sequential order and bypass values ────────────────
    // Using time-based base + offset keeps order values unique and naturally ordered.
    const auto baseTime = static_cast<int> (std::time (nullptr));
    for (size_t i = 0; i < newChain.size(); ++i)
    {
        settings->setValue (getKey ("order", newChain[i]), baseTime + static_cast<int> (i));
        if (i < bypassStates.size())
            settings->setValue (getKey ("bypass", newChain[i]), bypassStates[i]);
    }

    settings->saveIfNeeded();
    sortedCacheDirty = true;

    // ── Step 4: reload graph ─────────────────────────────────────────────────
    // If new plugins were added we need DLL loads — hand off to the background
    // thread (which handles the full chain).  For reorder/remove-only changes a
    // plain reconnect is sufficient.
    if (hasNewPlugins)
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

void IconMenu::removePluginsLackingInputOutput()
{
    const auto types = knownPluginList.getTypes();

    for (const auto& plugin : types)
    {
        if (plugin.numInputChannels < 2 || plugin.numOutputChannels < 2)
            knownPluginList.removeType (plugin);
    }
}
