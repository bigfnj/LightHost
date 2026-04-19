# AI_UNDERSTANDING.md
# LightHost — Machine-Readable Project Intelligence
# Generated: 2026-04-18 | Model: gpt-5.4
# Purpose: Full codebase context for future AI sessions. NOT human-documentation.

---

## META

```
project_name: Light Host
version: 3.8.1
release_date: 2026-04-17
author: BigFN'j
license: GPLv2+
original_author: Rolando Islas (2015)
bundle_id: com.bigfnj.lighthost
repo: https://github.com/BigFNj
language: C++23
build_system: CMake 4.3.1 + JUCE 8.0.12+
compiler: MSVC 19.50 (VS 2026)
target_os: Windows (primary), macOS, Linux
target_arch: x64
windows_sdk: 10.0.26100.0 (Win11)
local_git_repo: initialized (main, no commits yet)
```

---

## PURPOSE

Lightweight VST/AU/VST3 plugin host that runs as a system tray icon (Windows) or menu bar icon (macOS). Zero main window. Right-click tray icon → manage plugin chain. Left-click → open settings/edit plugins. Audio passes through a chained AudioProcessorGraph in real time.

---

## FILE TREE

```
/mnt/d/AI_Projects/@-LightHost/
├── .gitignore                  # Build/IDE ignore rules; keeps vendored lib/ trackable
├── .gitattributes              # LF defaults, CRLF for VS files, binary asset markers
├── CMakeLists.txt              # Build definition
├── CMakePresets.json           # VS, Ninja, Clang, and MinGW presets
├── AI_UNDERSTANDING.md         # This file
├── CHANGELOG.md                # Version history
├── gpl.txt                     # GPL full text
├── license                     # License header
├── third_party                 # Attribution file
├── Resources/
│   ├── icon.png                # App icon (.exe / bundle)
│   ├── MicrophoneIcon.png      # Tray icon (BinaryData compiled)
│   ├── menu_icon.png           # Menu icon variant
│   └── menu_icon_white.png     # Menu icon white variant
├── Source/
│   ├── HostStartup.cpp         # JUCEApplication entry point
│   ├── IconMenu.hpp            # IconMenu class declaration
│   ├── IconMenu.cpp            # IconMenu full implementation
│   ├── PluginWindow.h          # PluginWindow class declaration
│   ├── PluginWindow.cpp        # PluginWindow full implementation
│   ├── PreferencesWindow.h     # Preferences window declaration
│   └── PreferencesWindow.cpp   # Preferences window implementation
├── Utilities/
│   └── Reset Settings.command  # Convenience helper for clearing persisted settings
├── lib/
│   ├── juce/                   # Vendored JUCE framework
│   └── vstsdk2.4/              # Vendored Steinberg VST2.4 SDK
└── build/                      # Local generated build trees (ignored in git)
```

---

## BUILD SYSTEM

### CMakeLists.txt

```
cmake_minimum_required: 3.28
project: LightHost VERSION 3.8.1
languages: C, CXX
std: C++23 (REQUIRED, NO_EXTENSIONS)
CMAKE_SYSTEM_VERSION: 10.0.26100.0  # MUST be set BEFORE project() call
```

**Cross-platform note:**
```
C is enabled at the top level because JUCE pulls in C translation units
(notably juce_graphics_Sheenbidi.c) during native Linux/WSL builds.
Without LANGUAGES C CXX, Linux configure/generate fails at CMAKE_C_COMPILE_OBJECT.
```

**Compilation units:**
```
Source/HostStartup.cpp
Source/IconMenu.cpp
Source/PluginWindow.cpp
Source/PreferencesWindow.cpp
```

**Binary resource:**
```
juce_add_binary_data(LightHostData SOURCES Resources/MicrophoneIcon.png)
→ BinaryData::MicrophoneIcon_png (ptr)
→ BinaryData::MicrophoneIcon_pngSize (size_t)
```

**JUCE preprocessor flags:**
```
JUCE_WEB_BROWSER=0
JUCE_USE_CURL=0
JUCE_USE_FLAC=0
JUCE_USE_OGGVORBIS=0
JUCE_USE_CDBURNER=0
JUCE_USE_CDREADER=0
JUCE_PLUGINHOST_VST3=1
JUCE_PLUGINHOST_AU=1
JUCE_PLUGINHOST_VST=1
JUCE_MODAL_LOOPS_PERMITTED=1
JUCE_WASAPI=1
JUCE_DIRECTSOUND=1
JUCE_ALSA=1
JUCE_ASIO=1
```

**Windows platform defines:**
```
WINVER=0x0A00
_WIN32_WINNT=0x0A00
```

**Compiler flags (MSVC):**
```
/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /wd5105
Release: /O2 /GL /Gy + linker: /LTCG /OPT:REF /OPT:ICF
```

**Linked libraries:**
```
dwmapi                         # DWM for Win11 visuals
juce::juce_audio_basics
juce::juce_audio_devices
juce::juce_audio_formats
juce::juce_audio_processors
juce::juce_audio_utils
juce::juce_core
juce::juce_data_structures
juce::juce_events
juce::juce_graphics
juce::juce_gui_basics
juce::juce_gui_extra
juce::juce_recommended_config_flags
juce::juce_recommended_warning_flags
```

**VST SDK include path:** `lib/vstsdk2.4/`

### CMakePresets.json (version 6)

| preset | generator | config | output |
|--------|-----------|--------|--------|
| default | Visual Studio 18 2026 x64 | Debug | build/default/ |
| release | Visual Studio 18 2026 x64 | Release | build/release/ |
| ninja-debug | Ninja | Debug | build/ninja-debug/ |
| ninja-release | Ninja | Release | build/ninja-release/ |
| ninja-relwithdebinfo | Ninja | RelWithDebInfo | build/ninja-relwithdebinfo/ |
| clang-debug | Ninja + clang/clang++ | Debug | build/clang-debug/ |
| clang-release | Ninja + clang/clang++ | Release | build/clang-release/ |
| mingw-release | Ninja + MinGW-w64 cross toolchain | Release | build/mingw-release/ |

**Build presets:**
```
debug                 → default / Debug
release               → release / Release
ninja-debug           → ninja-debug
ninja-release         → ninja-release
ninja-relwithdebinfo  → ninja-relwithdebinfo
clang-debug           → clang-debug
clang-release         → clang-release
mingw-release         → mingw-release
```

### Git Readiness

```
git init --initial-branch=main run locally on 2026-04-18
.gitignore excludes generated outputs and IDE state, but does NOT blanket-ignore lib/
.gitattributes normalizes text files to LF by default and preserves Windows line endings
for Visual Studio project formats
```

---

## ARCHITECTURE

### Class Hierarchy

```
juce::JUCEApplication
  └── PluginHostApp                     HostStartup.cpp

juce::SystemTrayIconComponent
juce::Timer
juce::ChangeListener
  └── IconMenu                          IconMenu.hpp / IconMenu.cpp
        └── [nested] PluginListWindow   (inner class in IconMenu.cpp)

juce::DocumentWindow
  └── PluginWindow                      PluginWindow.h / PluginWindow.cpp
        └── [nested] ProgramAudioProcessorEditor
        └── [nested] ProgramPropertyComponent
```

### Dependency Graph

```
HostStartup.cpp
  → IconMenu.hpp
  → juce_gui_extra, juce_audio_utils

IconMenu.cpp
  → IconMenu.hpp
  → PluginWindow.h
  → BinaryData.h
  → <algorithm>, <ctime>
  → <windows.h>   (WIN32 only, for GetCursorPos)

PluginWindow.cpp
  → PluginWindow.h
  → juce_audio_utils, juce_gui_extra
```

---

## COMPONENT DETAILS

### HostStartup.cpp (82 lines)

**Global function (non-member):**
```cpp
juce::ApplicationProperties& getAppProperties()
  // Returns getApp().appProperties
  // Entry point for all settings access from other modules
```

**Class: PluginHostApp final : public juce::JUCEApplication**

Members:
```cpp
std::unique_ptr<juce::ApplicationProperties> appProperties
juce::LookAndFeel_V4 lookAndFeel
std::unique_ptr<IconMenu> iconMenu
```

Methods:
```cpp
void initialise(const juce::String& commandLine) override
  // Initializes ApplicationProperties (settings file)
  // Applies multi-instance suffix if -multi-instance=VALUE arg present
  // Sets default LookAndFeel
  // Creates IconMenu
  // macOS: hides dock icon

void shutdown() override
  // Resets iconMenu, appProperties, clears LookAndFeel

void systemRequestedQuit() override → quit()

const juce::String getApplicationName() override → "Light Host"
const juce::String getApplicationVersion() override → JUCE_APPLICATION_VERSION_STRING

bool moreThanOneInstanceAllowed() override → multiInstanceName.isNotEmpty()

[[nodiscard]] juce::String getMultiInstanceName() const
  // Parses -multi-instance=VALUE from commandLine args

void applyMultiInstanceSuffix(juce::PropertiesFile::Options& options) const
  // Appends multiInstanceName to settings filename suffix

static PluginHostApp& getApp()
  // Casts JUCEApplication::getInstance() → PluginHostApp (unchecked)
```

Entry point macro: `START_JUCE_APPLICATION(PluginHostApp)`

---

### IconMenu.hpp (70 lines)

**Global forward declaration:**
```cpp
juce::ApplicationProperties& getAppProperties();
```

**Class: IconMenu final : public juce::SystemTrayIconComponent, private juce::Timer, public juce::ChangeListener**

**Menu action ID constants:**
```cpp
static constexpr int kEditOffset     = 1'000'000
static constexpr int kBypassOffset   = 2'000'000
static constexpr int kDeleteOffset   = 3'000'000
static constexpr int kMoveUpOffset   = 4'000'000
static constexpr int kMoveDownOffset = 5'000'000
```

**Public methods:**
```cpp
IconMenu()
~IconMenu() override
void mouseDown(const juce::MouseEvent&) override
void changeListenerCallback(juce::ChangeBroadcaster* changed) override
[[nodiscard]] static juce::String getKey(const juce::String& type, const juce::PluginDescription& plugin)
  // Format: "plugin-{type}-{name}{version}{format}"
```

**Private members:**
```cpp
juce::AudioDeviceManager deviceManager
juce::AudioPluginFormatManager formatManager
juce::KnownPluginList knownPluginList           // all discovered plugins
juce::KnownPluginList activePluginList          // currently loaded chain
juce::KnownPluginList::SortMethod pluginSortMethod = sortByManufacturer
juce::PopupMenu menu
juce::AudioProcessorGraph graph
juce::AudioProcessorPlayer player
using NodeID = juce::AudioProcessorGraph::NodeID
NodeID inputNodeId                              // = 1'000'000
NodeID outputNodeId                             // = 1'000'001
bool menuIconLeftClicked = false
mutable std::vector<juce::PluginDescription> sortedPluginCache
mutable bool sortedCacheDirty = true
std::unique_ptr<PluginListWindow> pluginListWindow
```

**Private methods:**
```cpp
void timerCallback() override
void reloadPlugins()
void showPreferences()                  // replaced showAudioSettings() in v3.1.0
void applyPluginChain(const std::vector<juce::PluginDescription>&)
void loadActivePlugins()
void savePluginStates()
void deletePluginStates()
void setIcon()
void removePluginsLackingInputOutput()
void handleDeletePlugin(int index)
// handleAddPlugin(int) REMOVED in v3.3.0 — was dead code since v3.1.0
void handleBypassPlugin(int index)
void handleEditPlugin(int index)
void handleMovePlugin(int index, bool moveUp)
void onPluginInstanceReady(std::unique_ptr<juce::AudioProcessor>, NodeID, int generation)
void onAllPluginsLoaded(int generation)
[[nodiscard]] const std::vector<juce::PluginDescription>& getTimeSortedList() const
void reconnectGraph()
static void menuInvocationCallback(int id, IconMenu*)
```

---

### IconMenu.cpp (582 lines)

#### Nested class: IconMenu::PluginListWindow (lines 14–61)

```cpp
class IconMenu::PluginListWindow final : public DocumentWindow
```

Constructor `(IconMenu& owner_, AudioPluginFormatManager& pluginFormatManager)`:
- Title: "Available Plugins"
- Creates PluginListComponent (JUCE built-in)
- Loads dead man's pedal (crashed plugins list)
- Restores window geometry from settings["listWindowPos"]
- Resizable range: 300×400 → 800×1500

`closeButtonPressed()`:
- Calls owner.removePluginsLackingInputOutput()
- macOS: hides dock icon
- Resets owner.pluginListWindow = nullptr

#### IconMenu::IconMenu() constructor (lines 64–94)

```
adds default plugin formats to formatManager (VST, AU, VST3)
deviceManager.initialise(2, 2, savedAudioState, true)
  // 2-channel init — CRITICAL: was 256 before Session 1 fix
player.setProcessor(&graph)
deviceManager.addAudioCallback(&player)
loads "pluginList" XML → knownPluginList
knownPluginList.addChangeListener(this)
loads "pluginListActive" XML → activePluginList
loadActivePlugins()
activePluginList.addChangeListener(this)
setIcon()
setIconTooltip("Light Host")
```

#### ~IconMenu() destructor (v3.2.0+)
```
// MUST be first — deviceManager is declared before player/graph in member order,
// so it is destroyed LAST. Without removeAudioCallback(), the audio thread
// fires audioDeviceIOCallback() into player/graph while they are being destroyed.
deviceManager.removeAudioCallback(&player)
player.setProcessor(nullptr)
// Stop background load thread before any graph/settings access
if (pluginLoadThread != nullptr):
    pluginLoadThread->stopThread(3000)
    pluginLoadThread.reset()
savePluginStates()   // persist all plugin states before exit
// Members destroyed in reverse declaration order after destructor body exits:
// preferencesWindow, pluginListWindow, pluginLoadThread(already null), ...,
// player, graph, ..., deviceManager (last — safe since callback already removed)
```

#### setIcon() (lines 102–106)
```
ImageCache::getFromMemory(BinaryData::MicrophoneIcon_png, MicrophoneIcon_pngSize)
setIconImage(image, image)
```

#### loadActivePlugins() (lines 109–165)

```
sortedCacheDirty = true
PluginWindow::closeAllCurrentlyOpenWindows()
graph.clear()
inputNodeId  = graph.addNode(AudioGraphIOProcessor(audioInputNode),  NodeID{1'000'000})
outputNodeId = graph.addNode(AudioGraphIOProcessor(audioOutputNode), NodeID{1'000'001})
sorted = getTimeSortedList()
sampleRate = graph.getSampleRate() || 44100.0   // fallback
blockSize  = graph.getBlockSize()  || 512        // fallback
for each plugin in sorted:
  instance = formatManager.createPluginInstance(plugin, sampleRate, blockSize, err)
  if instance == null: log error, continue      // BUG FIX: was break
  stateData = settings["state" key] → base64 decode → setStateInformation()
  nodeId = settings["nodeid" key]
  if nodeId == 0:
    nodeId = settings["nextPluginNodeId"]++
    settings.setValue("nodeid" key, nodeId)
  graph.addNode(std::move(instance), NodeID{nodeId})
reconnectGraph()
```

#### reconnectGraph() (lines 168–230)

```
graph.removeAllConnections()
sorted = getTimeSortedList()
if sorted.empty():
  connect input[0] → output[0]
  connect input[1] → output[1]
  return
lastActiveNodeId = invalid
for each plugin in sorted:
  nodeId = settings["nodeid" key]
  node = graph.getNodeForId(NodeID{nodeId})
  bypassed = settings["bypass" key]
  if !bypassed && node != null:
    if no lastActiveNodeId:
      connect input[0] → node[0]
      connect input[1] → node[1]
    else:
      connect lastActiveNode[0] → node[0]
      connect lastActiveNode[1] → node[1]
    lastActiveNodeId = nodeId
if lastActiveNodeId valid:
  connect lastActiveNode[0] → output[0]
  connect lastActiveNode[1] → output[1]
else:
  connect input[0] → output[0]   // all bypassed
  connect input[1] → output[1]
```

#### getTimeSortedList() (lines 233–251)

```
if !sortedCacheDirty: return sortedPluginCache
types = activePluginList.getTypes()
std::sort(types, [](a, b) {
  orderA = settings["order" key for a]
  orderB = settings["order" key for b]
  return orderA < orderB
})
sortedPluginCache = types
sortedCacheDirty = false
return sortedPluginCache
```

**Complexity:** O(n log n), cached until dirty

#### changeListenerCallback() (lines 254–274)

```
if changed == &knownPluginList:
  xml = knownPluginList.createXml()
  settings.setValue("pluginList", xml)
else if changed == &activePluginList:
  xml = activePluginList.createXml()
  settings.setValue("pluginListActive", xml)
settings.saveIfNeeded()
```

#### getKey() static (lines 277–280)

```
return "plugin-" + type.toLowerCase() + "-" + plugin.name + plugin.version + plugin.pluginFormatName
examples:
  plugin-order-Delay110.0.0VST3
  plugin-bypass-Compressor2.5.0AU
  plugin-state-Reverb1.2.0VST
  plugin-nodeid-EQ0.9.5VST
```

#### timerCallback() (lines 283–340)

```
stopTimer(); menu.clear()
addSectionHeader(getApplicationName())
if menuIconLeftClicked:
  add "Preferences" id=1
  add "Edit Plugins" id=2
  addSeparator
  addSectionHeader "Active Plugins"
  for i, plugin in enumerate(getTimeSortedList()):
    submenu:
      "Edit"      id = kEditOffset     + i
      "Bypass"    id = kBypassOffset   + i  (ticked if bypassed)
      "Move Up"   id = kMoveUpOffset   + i  (disabled if i==0)
      "Move Down" id = kMoveDownOffset + i  (disabled if i==last)
      "Delete"    id = kDeleteOffset   + i
    addSubMenu(plugin.name, submenu)
  addSeparator
  addSectionHeader "Available Plugins"
  knownPluginList.addToMenu(menu, pluginSortMethod)  // adds remaining IDs
else (right-click):
  add "Quit" id=1
  addSeparator
  add "Delete Plugin States" id=2
// Windows: GetCursorPos() for menu position
menu.showMenuAsync(options, ModalCallbackFunction::forComponent(menuInvocationCallback, this))
```

#### mouseDown() (lines 343–352)

```
macOS: Process::setDockIconVisible(true)
Process::makeForegroundProcess()
menuIconLeftClicked = event.mods.isLeftButtonDown()
startTimer(50)   // defer menu display 50ms
```

#### menuInvocationCallback() static (lines 355–408)

```
if id == 0:  // menu dismissed
  macOS: if no active windows: Process::setDockIconVisible(false)
  return
// Single unified dispatch (v3.4.0+) — right-click only uses this path;
// left-click calls showPreferences() directly from mouseDown(), bypassing timer/menu entirely
id==1: showPreferences()
id==2: reloadPlugins()
id==3: savePluginStates(), quit()
id==4: deletePluginStates(), loadActivePlugins()
// plugin actions (by ID range):
if id in [kDeleteOffset, kDeleteOffset+kEditOffset):   handleDeletePlugin(id - kDeleteOffset)
if id in [kMoveDownOffset, ...):                       handleMovePlugin(id - kMoveDownOffset, false)
if id in [kMoveUpOffset, ...):                         handleMovePlugin(id - kMoveUpOffset, true)
if id in [kBypassOffset, ...):                         handleBypassPlugin(id - kBypassOffset)
if id in [kEditOffset, ...):                           handleEditPlugin(id - kEditOffset)
startTimer(50)  // re-show menu after plugin chain actions
```

#### handleDeletePlugin() (lines 411–438, v2.2.0)

```
plugin = getTimeSortedList()[index]
savePluginStates()                       // save surviving plugins' states first
nodeIdVal = settings["nodeid" key]
if nodeIdVal != 0:
  PluginWindow::closeCurrentlyOpenWindowsFor(NodeID{nodeIdVal})  // close UI
  graph.removeNode(NodeID{nodeIdVal})    // destroy ONLY this instance
settings.removeValue("order" key)
settings.removeValue("bypass" key)
settings.removeValue("state" key)
settings.removeValue("nodeid" key)
settings.saveIfNeeded()
activePluginList.removeType(plugin)      // triggers changeListener → persists XML
sortedCacheDirty = true
reconnectGraph()                         // rewire — 0 DLL loads
// NOTE: does NOT call loadActivePlugins() — O(0) plugin loads vs O(N-1)
```

#### handleAddPlugin() — REMOVED in v3.3.0
```
// This function was the handler for adding plugins from the tray left-click menu
// available-plugins list. That list was removed in v3.1.0 when plugin management
// moved to PreferencesWindow. The function was dead code (no call sites) from
// v3.1.0 onwards. Removed from both IconMenu.hpp and IconMenu.cpp in v3.3.0.
// Plugin addition now goes through: PreferencesWindow → applyPluginChain() → loadActivePlugins()
```

#### handleBypassPlugin() (lines 445–455)

```
plugin = getTimeSortedList()[index]
key = getKey("bypass", plugin)
current = settings.getBoolValue(key)
settings.setValue(key, !current)
reconnectGraph()    // OPTIMIZATION: was savePluginStates()+loadActivePlugins()
```

#### handleEditPlugin() (lines 457–467)

```
plugin = getTimeSortedList()[index]
nodeId = settings.getIntValue(getKey("nodeid", plugin))
if nodeId == 0: return
node = graph.getNodeForId(NodeID{nodeId})
PluginWindow::getWindowFor(node, PluginWindow::Normal)->toFront(true)
```

#### handleMovePlugin() (lines 469–500)

```
savePluginStates()
sorted = getTimeSortedList()
plugin = sorted[index]
// swap order keys with neighbor (index-1 if moveUp, index+1 if moveDown)
neighbor = sorted[moveUp ? index-1 : index+1]
pluginOrder   = settings.getIntValue(getKey("order", plugin))
neighborOrder = settings.getIntValue(getKey("order", neighbor))
settings.setValue(getKey("order", plugin),   neighborOrder)
settings.setValue(getKey("order", neighbor), pluginOrder)
sortedCacheDirty = true
reconnectGraph()    // OPTIMIZATION: was loadActivePlugins()
```

#### deletePluginStates() (lines 503–512)

```
for plugin in getTimeSortedList():
  settings.removeValue(getKey("state", plugin))
```

#### savePluginStates() (lines 514–535)

```
sorted = getTimeSortedList()
for plugin in sorted:
  nodeId = settings.getIntValue(getKey("nodeid", plugin))
  node = graph.getNodeForId(NodeID{nodeId})
  if node == null: continue         // BUG FIX: was break, lost all subsequent states
  MemoryBlock block
  node->getProcessor()->getStateInformation(block)
  b64 = block.toBase64Encoding()
  settings.setValue(getKey("state", plugin), b64)
settings.saveIfNeeded()
```

#### showAudioSettings() (lines 538–562)

```
AudioDeviceSelectorComponent selector(deviceManager, 0, 2, 0, 2, false, false, false, false)
selector.setSize(500, 270)
DialogWindow::LaunchOptions opts
opts.content = &selector
opts.dialogTitle = "Audio Settings"
opts.runModal()  // or launchAsync if !JUCE_MODAL_LOOPS_PERMITTED
xml = deviceManager.createStateXml()
settings.setValue("audioDeviceState", xml)
settings.saveIfNeeded()
```

#### reloadPlugins() (lines 564–570)

```
if pluginListWindow == null:
  pluginListWindow = make_unique<PluginListWindow>(*this, formatManager)
pluginListWindow->toFront(true)
```

#### removePluginsLackingInputOutput() (lines 572–581)

```
for plugin in knownPluginList.getTypes():
  if plugin.numInputChannels < 2 || plugin.numOutputChannels < 2:
    knownPluginList.removeType(plugin)
```

---

### PluginWindow.h (55 lines)

**Enum:**
```cpp
enum WindowFormatType { Normal = 0, Generic, Programs, NumTypes }
```

**Class: PluginWindow final : public juce::DocumentWindow**

Members:
```cpp
juce::AudioProcessorGraph::Node::Ptr owner
WindowFormatType type
```

Static helpers:
```cpp
static juce::Array<PluginWindow*>& getActiveWindows()  // static storage
```

Public interface:
```cpp
PluginWindow(juce::Component* pluginEditor, juce::AudioProcessorGraph::Node::Ptr, WindowFormatType)
~PluginWindow() override
static PluginWindow* getWindowFor(juce::AudioProcessorGraph::Node::Ptr, WindowFormatType)
static void closeCurrentlyOpenWindowsFor(juce::AudioProcessorGraph::NodeID nodeId)
static void closeAllCurrentlyOpenWindows()
[[nodiscard]] static bool containsActiveWindows()
void moved() override
void closeButtonPressed() override
```

Utility inline functions (outside class):
```cpp
inline juce::String toString(PluginWindow::WindowFormatType type)
  → "Normal" | "Generic" | "Programs" | "Parameters" | ""
inline juce::String getLastXProp(PluginWindow::WindowFormatType type)
  → "uiLastX_" + toString(type)
inline juce::String getLastYProp(PluginWindow::WindowFormatType type)
  → "uiLastY_" + toString(type)
inline juce::String getOpenProp(PluginWindow::WindowFormatType type)
  → "uiopen_" + toString(type)
```

---

### PluginWindow.cpp (178 lines)

#### getActiveWindows() (lines 6–10)
```cpp
static Array<PluginWindow*> windows;  // static, persists across calls
return windows;
```

#### PluginWindow constructor (lines 13–33)
```
DocumentWindow(pluginEditor->getName(), ...)
owner = o; type = t
setSize(400, 300)
setUsingNativeTitleBar(true)
setContentOwned(pluginEditor, true)
x = owner->properties[getLastXProp(type)] || Random::getSystemRandom().nextInt(500)
y = owner->properties[getLastYProp(type)] || Random::getSystemRandom().nextInt(500)
setTopLeftPosition(x, y)
owner->properties.set(getOpenProp(type), true)
setVisible(true)
getActiveWindows().add(this)
```

#### ~PluginWindow() (lines 35–39)
```
getActiveWindows().removeFirstMatchingValue(this)
clearContentComponent()
```

#### closeCurrentlyOpenWindowsFor() (lines 42–48)
```
for window in getActiveWindows() (reverse order):
  if window->owner->nodeID == nodeId:
    delete window
```

#### closeAllCurrentlyOpenWindows() (lines 50–64)
```
windows = getActiveWindows()
while windows.size() > 0:
  delete windows[windows.size()-1]
if JUCE_MODAL_LOOPS_PERMITTED:
  // dispatch loop 50ms to allow deletion messages
  dummy = new Component(); dummy->enterModalState()
  MessageManager::getInstance()->runDispatchLoopUntil(50)
  dummy->exitModalState(0); delete dummy
```

#### containsActiveWindows() (lines 66–69)
```
return getActiveWindows().size() > 0
```

#### Nested: ProgramPropertyComponent (lines 72–83)
```cpp
class ProgramPropertyComponent final : public PropertyComponent
  // Stub: refresh() does nothing
  // Displays program name in property panel
```

#### Nested: ProgramAudioProcessorEditor (lines 86–124)
```cpp
class ProgramAudioProcessorEditor final : public AudioProcessorEditor
  explicit ProgramAudioProcessorEditor(AudioProcessor* const p)
  // creates PropertyPanel
  // for each program: ProgramPropertyComponent(name)
  // size: 400 × clamp(totalHeight, 25, 400)
  void paint(Graphics& g) → fills with LookAndFeel bg color
  void resized() → panel.setBounds(getLocalBounds())
  PropertyPanel panel
```

#### getWindowFor() static (lines 127–164)
```
assert node != null
for w in getActiveWindows():
  if w->owner == node && w->type == type: return w  // already open
// create new window
Component* ui = null
if type == Normal:
  ui = processor->createEditorIfNeeded()
  if ui == null: type = Generic
if ui == null:
  if type == Generic || type == Parameters:
    ui = new GenericAudioProcessorEditor(*processor)   // JUCE 8: takes reference
  else if type == Programs:
    ui = new ProgramAudioProcessorEditor(processor)
if ui != null && processor is AudioPluginInstance:
  ui->setName(plugin->getName())
return new PluginWindow(ui, node, type)  // or nullptr if ui == null
```

#### moved() (lines 167–171)
```
owner->properties.set(getLastXProp(type), getX())
owner->properties.set(getLastYProp(type), getY())
```

#### closeButtonPressed() (lines 173–177)
```
owner->properties.set(getOpenProp(type), false)
delete this
```

---

## SETTINGS PERSISTENCE

**Storage:** ApplicationProperties → PropertiesFile (XML-based, OS config dir)
- macOS: `~/Library/Preferences/`
- Windows: `%APPDATA%/BigFNj/Light Host/`
- Multi-instance: filename suffix appended (e.g., `LightHost-instanceA.settings`)

**Settings keys:**
```
"audioDeviceState"                              → XML: audio device config
"pluginList"                                    → XML: KnownPluginList (all discovered)
"pluginListActive"                              → XML: KnownPluginList (active chain)
"listWindowPos"                                 → String: plugin browser window geometry
"plugin-order-{name}{version}{format}"          → int: Unix timestamp for sort order
"plugin-bypass-{name}{version}{format}"         → bool: bypass state
"plugin-state-{name}{version}{format}"          → String: base64 plugin binary state
"plugin-nodeid-{name}{version}{format}"         → int: stable NodeID for graph
"nextPluginNodeId"                              → int: counter for NodeID generation
"uiLastX_{Normal|Generic|Programs}"             → int: last X of plugin window
"uiLastY_{Normal|Generic|Programs}"             → int: last Y of plugin window
"uiopen_{Normal|Generic|Programs}"              → bool: window open state
```

**Reserved NodeIDs:**
```
1'000'000 = input node
1'000'001 = output node
user plugins start at 1 (nextPluginNodeId counter)
```

---

## DATA FLOW

```
DISCOVERY:
  PluginListWindow → PluginListComponent
  → scan with formatManager
  → knownPluginList.addType()
  → changeListener → settings["pluginList"]

ACTIVATION (v3.1.0+):
  user adds plugin via PreferencesWindow → clicks Apply
  → applyPluginChain(newChain)
  → if new plugins: loadActivePlugins() (background thread)
  → else: reconnectGraph() only

INSTANTIATION:
  loadActivePlugins()
  → graph.clear()
  → create I/O nodes (id 1M, 1M+1)
  → for each plugin: createPluginInstance()
  → restore state from base64 settings
  → assign/retrieve stable NodeID
  → graph.addNode(instance, NodeID)
  → reconnectGraph()

WIRING:
  reconnectGraph()
  → graph.removeAllConnections()
  → chain: input → [p0] → [p1] → ... → output
  → skip bypassed nodes
  → direct passthrough if all bypassed

AUDIO PROCESSING:
  deviceManager → AudioProcessorPlayer → AudioProcessorGraph
  → processes plugin chain on audio thread
  → output to hardware

PERSISTENCE:
  savePluginStates()
  → for each plugin: getStateInformation() → base64 → settings
  → saveIfNeeded() → disk

SHUTDOWN:
  ~IconMenu() → savePluginStates()
```

---

## DESIGN PATTERNS

| Pattern | Usage |
|---------|-------|
| Observer | IconMenu → ChangeListener on knownPluginList, activePluginList |
| Singleton | getActiveWindows() static array; ApplicationProperties single instance |
| Factory | AudioPluginFormatManager creates plugin instances; PluginWindow::getWindowFor creates UI |
| State | plugin bypass, order, binary state persisted in settings |
| Cache | sortedPluginCache + sortedCacheDirty flag, invalidated on list changes |
| Graph | AudioProcessorGraph as DAG of plugin nodes with audio edge connections |

---

## THREADING MODEL

```
Message Thread:
  - UI updates
  - Menu display
  - loadActivePlugins(), reconnectGraph(), savePluginStates()
  - Plugin graph mutations

Audio Thread (real-time):
  - AudioProcessorPlayer callback
  - AudioProcessorGraph::processBlock()
  - Plugin parameter reads

Synchronization:
  - JUCE MessageManager serializes message thread ops
  - AudioProcessorGraph manages audio thread safety for graph changes
  - Plugin parameter changes from UI go through JUCE audio parameter system
```

---

## PERFORMANCE

### Session 1 Optimizations (v2.1.0)

| Issue | Before | After | Impact |
|-------|--------|-------|--------|
| Audio buffer channels | 256×256 | 2×2 | ~130x less memory bandwidth, eliminates idle CPU |
| Plugin sort | O(n²) re-read settings per pass | O(n log n) std::sort, cached | ~7x faster for 20 plugins |
| Bypass toggle | savePluginStates()+loadActivePlugins() | reconnectGraph() only | ~20x faster |
| Plugin move | loadActivePlugins() (destroy/recreate) | reconnectGraph() only | ~20x faster |
| Menu position (Windows) | cached stale cursor position | GetCursorPos() on each call | fixes wrong-position bug |
| Startup plugin loading | N DLL loads on message thread (UI frozen) | PluginLoadThread (worker thread) + callAsync | tray icon appears instantly; plugins load in background |
| Add plugin | loadActivePlugins() (N+1 DLL loads) | createPluginInstance() × 1 | eliminates UI freeze on add |
| Delete plugin | loadActivePlugins() (N-1 DLL loads) | graph.removeNode() only | eliminates UI freeze on delete |
| Sort order reads | O(N log N) settings reads in comparator | O(N) pre-read, O(1) int compare | ~log N fewer reads per sort |
| Move plugin | savePluginStates() + O(N) writes + O(N) key allocs | 2 value swap, 4 key allocs, no state save | near-instant for any chain length |
| Sorted list access in handlers | vector<PluginDescription> copy | const reference into cache | zero copy overhead |
| timerCallback settings access | getUserSettings() per plugin in loop | hoisted once before loop | 1 call vs N calls |

---

## BUG HISTORY

### Fixed in v3.3.0

7. **Duplicate Generic windows on second Edit click**
   - File: PluginWindow.cpp, getWindowFor()
   - Root cause: existing-window search matched on `type == Normal`; fallback windows are stored with `type == Generic`. Second call with Normal missed the existing Generic window and created a duplicate — two GenericAudioProcessorEditor instances on the same AudioProcessor, two listeners registered.
   - Fix: loop now also matches Generic when requested type is Normal: `w->type == type || (type == Normal && w->type == Generic)`

### Fixed in v3.2.0

6. **Crash on system shutdown / restart**
   - File: IconMenu.cpp, ~IconMenu()
   - Root cause: destructor never called `deviceManager.removeAudioCallback(&player)`. In C++ member destruction order (reverse of declaration), `player` and `graph` are destroyed before `deviceManager`. The audio device remained open, firing `audioDeviceIOCallback()` into freed player/graph memory — use-after-free on every exit path. Reliably fatal during system shutdown where the OS disrupts audio services in parallel.
   - Fix: `deviceManager.removeAudioCallback(&player)` + `player.setProcessor(nullptr)` added as the first two operations in the destructor body, before any other cleanup.

### Fixed in v2.1.0

1. **Plugin state corruption on delete**
   - File: IconMenu.cpp, handleDeletePlugin()
   - Root cause: deletePluginStates() called before savePluginStates() — wiped all states, then re-saved only deleted plugin
   - Fix: savePluginStates() first, then remove only deleted plugin's keys

2. **Silent plugin state loss on load failure**
   - File: IconMenu.cpp, loadActivePlugins()
   - Root cause: `break` on first null instance → all subsequent plugins lost states
   - Fix: `continue` instead of `break`

3. **Menu position bug (Windows)**
   - File: IconMenu.cpp, timerCallback()
   - Root cause: cursor position cached from mouseDown, menu displayed at wrong location
   - Fix: call GetCursorPos() fresh inside timerCallback

4. **Unsafe sample rate / block size**
   - File: IconMenu.cpp, loadActivePlugins()
   - Root cause: graph.getSampleRate() returns 0 before audio device init
   - Fix: fallback to 44100.0 / 512

5. **CMAKE_SYSTEM_VERSION not picked up**
   - File: CMakeLists.txt
   - Root cause: set after project() call; VS generator ignores it
   - Fix: moved before project()

6. **Stable NodeID system**
   - File: IconMenu.cpp/hpp
   - Root cause: plugin NodeIDs regenerated on every reload → windows lose reference, states orphaned
   - Fix: NodeID persisted in settings per plugin, reused on reload — enables reconnectGraph-only moves

---

## KNOWN LIMITATIONS

```
- No latency compensation
- Mono plugins filtered out (stereo-only)
- No side-chain routing
- No multi-instance per plugin type
- No MIDI routing
- No undo/redo for chain edits
- No named preset snapshots
- No plugin CPU metering
- No chain import/export
```

---

## PLATFORM SPECIFICS

### Windows
```
Audio: WASAPI (primary), DirectSound (fallback), ASIO (optional)
Tray menu: GetCursorPos() for position, native Win32 menu
SDK: 10.0.26100.0 (Win11)
DWM: linked for visual enhancements
WINVER: 0x0A00
```

### macOS
```
Audio: Core Audio
Plugin formats: VST, VST3, AU
Dock icon: hidden by default, shown when plugin windows open
Menu bar integration
```

### Linux
```
Audio: ALSA (primary)
Plugin formats: VST, VST3
Graphics: X11/Wayland via JUCE
```

---

## BUILD ARTIFACTS

```
Preset output roots:
  build/default/               # Visual Studio Debug
  build/release/               # Visual Studio Release
  build/ninja-debug/           # Native Ninja Debug
  build/ninja-release/         # Native Ninja Release
  build/ninja-relwithdebinfo/  # Native Ninja RelWithDebInfo
  build/clang-debug/           # Clang Debug
  build/clang-release/         # Clang Release
  build/mingw-release/         # Windows cross-compiled release via MinGW-w64

Primary Windows binary name:
  Light Host.exe

Workspace status:
  build/ninja-release/ exists locally from WSL/Linux configure testing

WSL/Linux note:
  Native JUCE configure requires ALSA/X11/font development headers on the host.
```

---

## CHANGELOG SUMMARY

```
Unreleased (2026-04-18)
  - Stability: FileLogger added at startup for persistent crash/silent-exit breadcrumbs
  - Stability: plugin state restore moved from PluginLoadThread worker thread to message thread
  - Stability: cancelPluginLoading() added before destructive graph mutations
  - Stability: applyPluginChain() now early-outs when Apply changed only device settings
  - Stability: device-name validation + setAudioDeviceSetup() error logging in Preferences Apply flow
  - Stability: exception guards around plugin state save/restore and plugin editor creation
  - Verification: native WSL/Linux debug configure + build completed successfully

v3.8.1 (2026-04-17)
  - Feature: version label bottom-left of Preferences window (versionLabel, 11pt, 45% alpha)
  - Feature: right-click any chain row in Preferences → "Delete" popup removes plugin from staged list
  - Feature: "Run at Startup" in right-click tray menu (Windows only) — creates/removes
    Light Host.lnk in FOLDERID_Startup via IShellLink COM; tick reflects current state;
    no registry access; #include <shlobj.h> added; three new private methods on IconMenu

v3.8.0 (2026-04-17)
  - Feature: dynamic Mono/Stereo channel label — queries device->getInputChannelNames().size()
  - Feature: "Scan Custom Folder..." injected into PluginListComponent Options popup
  - Feature: Device API dropdown in DEVICE SETTINGS (WASAPI shared vs. exclusive)
  - Feature: refreshPreferencesIfOpen() — tray delete/bypass/move refreshes Preferences chain list
  - Fix: staged apply-only device settings — all combos committed via Apply button only;
    Apply greys out during device restart via callAsync + SafePointer

v3.7.0 (2026-04-17)
  - Feature: redesigned Preferences UI — single signal-flow view replacing two-tab layout
  - INPUT/OUTPUT device combos (stereo-only, Ch 1-2), DEVICE SETTINGS always visible
  - AudioChainListComponent with bypass checkbox + [Edit] button + drag-to-reorder
  - "+ Add Plugin" manufacturer-grouped popup replaces left-panel AvailablePluginsListComponent
  - applyPluginChain() now delivers and persists bypass states alongside chain order

v3.6.0 (2026-04-17)
  - Feature: explicit sample rate auto-match on device change
  - autoMatchSampleRate(): validates current rate against device's getAvailableSampleRates();
    selects preferred rate (48k→44.1k→96k→88.2k→192k→32k→last) via setAudioDeviceSetup()
  - changeListenerCallback() deviceManager branch: re-entrant guard + reconnectGraph() +
    autoMatchSampleRate() + audio state persisted to settings on every device change
  - isHandlingDeviceChange: replaced manual flag set/clear with ScopedValueSetter<bool>
    (exception-safe; flag guaranteed to clear on scope exit)
  - showPreferences() onClose lambda: preferencesWindow.reset() deferred via callAsync
    (was UB — destroying std::function that owns the currently-executing lambda)

v3.5.0 (2026-04-17)
  - Feature: mono input auto-detected and duplicated to both stereo output channels
  - reconnectGraph() fallback: addConnection(ch1) returns false on mono → duplicate ch0
  - deviceManager added as ChangeListener → graph rewires on device switch in Preferences

v3.4.0 (2026-04-17)
  - Feature: left-click opens Preferences directly; right-click shows unified combined menu
  - Removed: menuIconLeftClicked member, dual-branch menu logic; menu IDs 3+4 for Quit/Delete

v3.3.0 (2026-04-17)
  - Bug fix: duplicate Generic PluginWindow on second Edit click (PluginWindow.cpp)
  - Dead code removal: handleAddPlugin() (declaration + implementation), dead since v3.1.0
  - Cleanup: redundant onChange re-assignment in syncLists() (PreferencesWindow.cpp)

v3.2.0 (2026-04-17)
  - Critical bug fix: crash on system shutdown/restart — audio callback teardown race
    (deviceManager.removeAudioCallback + player.setProcessor(nullptr) in ~IconMenu())

v3.1.0 (2026-04-16)
  - Feature: PreferencesWindow with Audio tab (AudioDeviceSelectorComponent) and
    Plugins tab (drag-to-reorder active chain, available plugins list, Apply button)
  - Replaces flat tray menu for plugin management; all changes staged before Apply

v3.0.0 / v2.4.0 (2026-04-16)
  - Background plugin loading (PluginLoadThread) — tray icon appears instantly
  - Generation counter + SafePointer for safe concurrent load cancellation
  - Release build: VS 2026, MSVC 19.50, CMake 4.3.1, /O2 /GL /LTCG

v2.3.0 (2026-04-16)
  - getTimeSortedList O(N log N) reads → O(N); handleMovePlugin O(N) writes → O(1) swap
  - handleBypassPlugin/handleDeletePlugin: vector copy → const ref
  - timerCallback: settings pointer hoisted outside plugin loop

v2.2.0 (2026-04-16)
  - handleAddPlugin: N+1 DLL loads → 1 DLL load (no loadActivePlugins() on add)
  - handleDeletePlugin: N-1 DLL loads → 0 (graph.removeNode + reconnectGraph)

v2.1.0 (2026-04-14)
  - Performance: 2-channel audio init, sorted plugin cache, bypass/move use reconnectGraph only
  - Bug fixes: state corruption on delete, silent break on load failure, menu position (Win),
    safe sample rate fallback, stable NodeID persistence
  - Build: VS 2026, MSVC 19.50, CMake 4.3.1, /O2 /GL /LTCG, zero warnings

v2.0.x and earlier: original Rolando Islas codebase, refactored by BigFN'j
```

---

## SESSION LOG

```
Session 1 (2026-04-14): Performance audit and bug fixes → v2.1.0
  Changes: audio buffer channels, sort cache, bypass/move optimization,
           stable NodeIDs, 5 bug fixes, build system cleanup

Session 2 (2026-04-16): Full codebase documentation → AI_UNDERSTANDING.md
  No code changes. Understanding written for future AI context.

Session 3 (2026-04-16): Fix add/remove plugin UI freeze → v2.2.0
Session 4 (2026-04-16): Second-pass optimizations → v2.3.0
Session 5 (2026-04-16): Background plugin loading → v2.4.0
  PluginLoadThread (juce::Thread) runs createPluginInstance on worker thread.
  callAsync delivers each instance to message thread. Audio passes through during load.
  SafePointer + generation counter for safe teardown and load cancellation.
  Destructor stops thread before savePluginStates.

Session 6 (2026-04-16): PreferencesWindow feature → v3.1.0
  New: PreferencesWindow.h/.cpp — Audio tab + Plugins tab with drag-to-reorder.
  applyPluginChain() replaces handleAddPlugin() as the plugin chain commit path.
  showPreferences() replaces showAudioSettings(). Available-plugins removed from tray menu.
  handleAddPlugin() became dead code at this point (call sites removed from menuInvocationCallback).

Session 7 (2026-04-17): Critical shutdown crash fix → v3.2.0
  ~IconMenu(): added deviceManager.removeAudioCallback(&player) + player.setProcessor(nullptr)
  as first two destructor operations. Without this, deviceManager (declared first, destroyed last)
  kept firing audioDeviceIOCallback() into player/graph memory during member destruction.
  Crash was reliably fatal on system restart/shutdown where OS disrupts audio in parallel.

Session 8 (2026-04-17): Production readiness audit → v3.3.0
  Bug fix: PluginWindow::getWindowFor() — duplicate Generic window on second Edit click for
    plugins with no native editor (Normal→Generic fallback not matched in existing-window search).
  Dead code removal: handleAddPlugin() declaration (IconMenu.hpp) + implementation (IconMenu.cpp).
  Cleanup: redundant activeList.onChange re-assignment in PluginsTabComponent::syncLists().
  AI_UNDERSTANDING.md: full update to reflect current codebase state.

Session 10 (2026-04-17): Mono input → stereo duplication → v3.5.0
  reconnectGraph(): all three input-node connection sites now attempt stereo ch1 first;
    if addConnection() returns false (mono device, ch1 out of range), duplicate ch0→ch1.
  changeListenerCallback(): added deviceManager branch → reconnectGraph() on device switch.
  Constructor: deviceManager.addChangeListener(this) after addAudioCallback.
  Destructor: deviceManager.removeChangeListener(this) before removeAudioCallback.
  No change to initialise(2,2,...) — JUCE already clamps to device channel count.

Session 12 (2026-04-17): Redesigned Preferences UI (signal-flow single view) → v3.7.0
  Removed: AudioDeviceSelectorComponent, TabbedComponent, AvailablePluginsListComponent,
    PluginsTabComponent — replaced with a single unified layout.
  New SectionLabel component: dark header bar for INPUT/AUDIO CHAIN/OUTPUT/DEVICE SETTINGS.
  New AudioChainListComponent: checkbox (bypass toggle, blue-filled when active), plugin name
    (dimmed when bypassed), [Edit] button (opens plugin UI via onEditClicked callback),
    drag handle (☰) for reordering. Bypass state travels with plugin during drag-reorder.
  PreferencesContentComponent: INPUT/OUTPUT combos built from AudioIODeviceType::getDeviceNames();
    changes call setAudioDeviceSetup() immediately. Stereo-only channel label (no Ch 3-8 exposure).
    DEVICE SETTINGS: sample rate combo (e.g. "48 kHz") + buffer size combo with ms latency
    (e.g. "512 samples  (10.7 ms)") — always visible, no advanced settings toggle.
    Registers as ChangeListener on AudioDeviceManager to keep combos in sync.
    suppressComboCbs guard prevents onChange re-entrancy during programmatic combo refresh.
    "+ Add Plugin" button opens manufacturer-grouped PopupMenu for adding plugins to chain.
  PreferencesWindow: added bypassStates + onEditPlugin parameters; window 520×560.
  IconMenu: applyPluginChain() now accepts bypassStates and persists them in settings Step 3.
    showPreferences() builds initial bypassStates from settings and passes to window.
    New openPluginEditorFor(PluginDescription&) looks up nodeId, calls PluginWindow::getWindowFor.

Session 11 (2026-04-17): Sample rate auto-match on device change + safety audit → v3.6.0
  New method autoMatchSampleRate(): queries device->getAvailableSampleRates(); if current
    rate unsupported, walks priority list (48k→44.1k→96k→88.2k→192k→32k) and calls
    deviceManager.setAudioDeviceSetup(setup, true). Falls back to available.getLast().
  changeListenerCallback() deviceManager branch expanded: isHandlingDeviceChange re-entrant
    guard (prevents setAudioDeviceSetup → changeCallback → setAudioDeviceSetup loops) +
    reconnectGraph() + autoMatchSampleRate() + deviceManager.createStateXml() persisted.
  isHandlingDeviceChange guard switched to ScopedValueSetter<bool> — exception-safe.
  showPreferences() onClose lambda: preferencesWindow.reset() deferred via callAsync —
    eliminates UB from destroying std::function (and contained lambda) during its own execution.
  IconMenu.hpp: added isHandlingDeviceChange member + autoMatchSampleRate() declaration.
  Plugin chain remains stereo-only; mono duplication is handled at the input node only.

Session 9 (2026-04-17): Unified menu UX feature → v3.4.0
  Left-click: mouseDown() calls showPreferences() directly; no timer, no menu.
  Right-click: single combined menu — Preferences, Edit Plugins, active plugin chain,
    separator, Quit (id=3), Delete Plugin States (id=4).
  Removed: menuIconLeftClicked member (IconMenu.hpp), dual-branch logic in timerCallback()
    and menuInvocationCallback(). Menu IDs 3+4 replace old right-click-only IDs 1+2.

Session 14 (2026-04-17): Version label, right-click delete in chain, Run at Startup → v3.8.1
  Version label: juce::Label versionLabel added to PreferencesContentComponent, positioned
    bottom-left in the Apply button row. getText from JUCEApplication::getApplicationVersion().
  Right-click delete: AudioChainListComponent::mouseDown() checks isRightButtonDown() first;
    shows PopupMenu with "Delete"; SafePointer guards async callback. Removes from items +
    bypassed, calls onChange, repaints.
  Run at Startup: Windows-only. IconMenu gets getStartupShortcutPath() (static, uses
    SHGetKnownFolderPath(FOLDERID_Startup)), isStartupEnabled() (existsAsFile check),
    setStartupEnabled(bool) (IShellLink COM create or deleteFile). Menu item id=5 in
    timerCallback() with tick; dispatched in menuInvocationCallback(). shlobj.h added.
    Most Vexing Parse fix: juce::File folder{juce::String(path)} not (juce::String(path)).

Session 13 (2026-04-17): Device API dropdown, staged apply, scan folder, chain refresh → v3.8.0
  Dynamic channel labels: updateChannelLabels() reads device->getInputChannelNames().size(),
    sets "Mono (Ch 1)" or "Stereo (Ch 1-2)" after every rebuildDeviceCombos() call.
  Scan Custom Folder: AugmentedPluginListComponent subclasses PluginListComponent, replaces
    getOptionsButton().onClick with lambda that appends "Scan Custom Folder..." (ID 9999) to
    existing Options menu. ScanJob (ThreadWithProgressWindow) runs PluginDirectoryScanner for
    each format against the user-chosen directory. browseAndScan() uses FileChooser::launchAsync.
  Device API dropdown: deviceTypeCombo added to DEVICE SETTINGS section, populated from
    deviceManager.getAvailableDeviceTypes(). Allows switching WASAPI shared ↔ exclusive.
    Shared mode (Windows Audio) eliminates crackling from USB device clock drift.
  refreshPreferencesIfOpen(): builds chain+bypass from settings, calls
    preferencesWindow->refreshPluginChain(). Called from handleDeletePlugin(),
    handleBypassPlugin(), handleMovePlugin() so the open Preferences window stays in sync.
  Staged apply-only device settings: removed all onChange callbacks from device combos.
    commitAllSettings() captures all combo values, calls callAsync so Apply button greys out
    before device restarts. SafePointer guards lambda against component destruction.
    Removed: suppressComboCbs, ScopedValueSetter guards, applyDeviceType/Change/SampleRate/BufferSize.

Session 15 (2026-04-18): Repo prep + inclusive presets + Linux native configure support
  CMakeLists.txt: project() now declares LANGUAGES C CXX so JUCE C sources configure correctly
    on Linux/WSL (notably juce_graphics_Sheenbidi.c).
  CMakePresets.json: expanded to include VS 2026 debug/release, native Ninja debug/release,
    RelWithDebInfo, Clang debug/release, and MinGW-w64 Windows cross-release presets.
  Repo prep: added .gitattributes, refined .gitignore to keep vendored lib/ sources trackable,
    and initialized a local git repository on branch main.
  WSL environment note: libasound2-dev plus X11/font development headers were installed to
    support native JUCE dependency detection and build debugging.

Session 16 (2026-04-18): Crash hardening audit for Preferences / Apply / plugin editors
  HostStartup.cpp: FileLogger added via createDateStampedLogger(); startup/shutdown now log.
  IconMenu.cpp/hpp: cancelPluginLoading() added; structural chain changes cancel background loads
    before graph mutation. Plugin load begin/end/failure and editor/delete/device-change actions log.
  PluginLoadThread no longer restores plugin state on the worker thread; saved state now applies on
    the message thread inside onPluginInstanceReady(...), reducing thread-affinity risk with plugins.
  applyPluginChain(): compares requested chain+bypass state to persisted state and early-outs when
    Apply changes only device settings, avoiding unnecessary savePluginStates() and graph churn.
  PreferencesWindow.cpp: Apply path now validates selected device names against the active device
    type and logs any setAudioDeviceSetup() error strings.
  PluginWindow.cpp: plugin editor creation wrapped in exception guards; failure logs instead of
    assuming all plugin editors construct safely.
  Verification: cmake --preset ninja-debug + cmake --build build/ninja-debug -j2 completed.
```
