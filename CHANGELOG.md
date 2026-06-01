# Changelog

---

## [Unreleased]

---

## [4.0.3] — 2026-06-01

### Production hardening — exception safety around long-lived state

Closes the v4.0.0 production-hardening backlog: four defensive fixes wrapping
calls that cross trust boundaries (third-party plugin code, JUCE settings
serialisation, audio device drivers) so a single throwing call no longer
takes down the host.

- **`pluginLoadGeneration` is now `std::atomic<int>`** in `IconMenu.hpp`. The
  counter cancels in-flight background plugin loads when the user reloads or
  changes the chain. Although IconMenu's reads/writes were already
  message-thread-only, making the type explicit matches its cross-thread
  semantics (the worker thread captures the value at start) and protects
  against any future code that might read it from a non-message thread.
- **`savePluginStates()`** now wraps the trailing `settings->saveIfNeeded()`
  in try-catch. The inner `getStateInformation` call (the throwing call —
  plugins can implement it badly) was already protected; wrapping
  `saveIfNeeded` itself is symmetric and cheap.
- **`activePluginList.addType/removeType`** are wrapped at all three
  callsites — `handleDeletePlugin`, and both branches of `applyPluginChain`.
  These trigger a `ChangeListener` callback that writes the XML plugin list
  to disk; protecting against any I/O- or XML-related throw avoids leaving
  the active list in a half-mutated state.
- **`dm.setCurrentAudioDeviceType` and `dm.setAudioDeviceSetup`** in the
  Preferences Apply path are wrapped in try-catch. ASIO and WASAPI drivers
  occasionally throw on device-switch race conditions; the existing
  unconditional Apply re-enable at the end of the lambda already covered
  the UI-recovery half of the fix, but exception protection was missing.
- **Cleanup**: removed a leftover `DBG(...)` line in `PluginLoadThread::run`
  that duplicated the canonical `juce::Logger::writeToLog` call. DBG is a
  debug-only no-op so this is a release-build noop.

All exception handlers log via `juce::Logger::writeToLog`, so failures show
up in the LightHost log file rather than silently corrupting state or
crashing the host.

### Logging — bounded file size + audio-config diagnostics

- **Log file is now bounded.** `HostStartup.cpp` previously used
  `FileLogger::createDateStampedLogger`, which created a fresh log file per
  launch — file count grew without limit. The logger is now a single
  rotating `LightHost.log` at the JUCE system log folder
  (`%APPDATA%\Light Host\LightHost.log` on Windows). `juce::FileLogger`
  trims the file to 256 KB on every open, so size is bounded across an
  arbitrary number of sessions. Sessions are visually separable inside
  the file via a `==== Light Host vX.Y.Z starting at <time> ====` banner.
- **Audio configuration is now logged at every transition.** A new
  `IconMenu::logAudioConfig(contextLabel)` helper writes a one-line summary
  of the active audio driver, input/output device names, active-vs-total
  channel counts, sample rate, and buffer size. Called once at startup
  after `deviceManager.initialise` and again from `changeListenerCallback`
  on every device change, so the log captures the current routing state at
  every transition. Useful for diagnosing routing-related issues (e.g.
  Teams noise-suppression bypass on virtual-cable outputs).

---

## [4.0.2] — 2026-05-30

### Bugfix — Lane Changes Silently Discarded by Apply

- `IconMenu::applyPluginChain()` had a "skip Apply if nothing changed"
  optimization that compared only the plugin order and bypass states between
  the current and requested chains. Lane assignments were not included in the
  comparison, so when a user changed only a lane dropdown and clicked Apply,
  the function detected no change and returned early — **before** writing the
  new lane values to settings. On the next launch the lane reverted to its
  previously persisted value.
- The fix extends the comparison to also detect lane mismatches, so Apply
  proceeds and `settings->setValue(getKey("lane", ...), ...)` is reached for
  every plugin whose lane was changed.

---

## [4.0.1] — 2026-05-28

### Dependency — JUCE 8.0.12 → 8.0.13

- Updated vendored JUCE to **8.0.13** (released 2026-05-19). Brings improved Windows
  rendering performance, improved Component painting performance, faster
  `juce_gui_basics` compile times, and reduced stack size for `Component` and
  `ListenerList` — meaningful gains for our primary Windows platform and our
  custom-painted Preferences chain list with lane dropdowns.
- API: replaced the deprecated `AudioProcessor::createEditorIfNeeded()` call in
  `PluginWindow::getWindowFor()` with `createEditorAndMakeActive()`. The new method
  also registers the editor pointer back on the `AudioProcessor` so
  `getActiveEditor()` returns the correct result. No behavioural change for
  LightHost — we already store our own pointer to the editor.

---

## [4.0.0] — 2026-05-28

### Feature — Parallel Processing Lanes + Plugin Delay Compensation

#### Parallel Lanes (`Source/IconMenu.cpp`, `Source/PreferencesWindow.cpp`, `Source/PreferencesWindow.h`)

- Each plugin can now be assigned to one of four parallel processing lanes (Lane 0–3) via
  a per-row dropdown in the Preferences chain list. Plugins on the same lane execute in
  sequence; lanes run in parallel and sum into the output node.
- The graph wiring in `IconMenu::reconnectGraph()` groups nodes by lane via a `std::map<int, LaneInfo>`,
  then connects `input → lane[0..n] → output` per lane. The JUCE `AudioProcessorGraph` handles
  the topological ordering and the implicit summing at the output node.
- Lane assignments are persisted per-plugin as `plugin-lane-<plugin-key>` integer entries in the
  settings file, alongside the existing order/bypass/state values.
- The `AudioChainListComponent` now carries a third companion vector (`lanes`) maintained in
  lockstep with `items` and `bypassed`. A central `syncBypassedSize()` helper enforces the invariant
  across all mutation sites (add, delete, drag-reorder).

#### Plugin Delay Compensation (`Source/DelayProcessor.hpp`, `Source/IconMenu.cpp`)

- New `DelayProcessor` audio processor — a lightweight 2-channel circular-buffer delay that buffers
  audio by exactly N samples per `prepareToPlay`. Reports zero latency itself; transparently
  inserted into the graph for PDC compensation only.
- `reconnectGraph()` now sums per-lane plugin latencies (`getLatencySamples()`), computes the
  maximum across lanes, and inserts a `DelayProcessor` node on each shorter lane before its
  output connection so all lanes arrive sample-aligned at the output node.
- Bypassed plugins contribute zero latency by design (matches DAW convention — PDC compensates
  the active processing path only).

### Stability — QA Hardening of Lanes + PDC Implementation

#### `DelayProcessor` Node Lifecycle (`Source/IconMenu.cpp`)

- Fixed memory leak: `reconnectGraph()` now sweeps and removes existing `DelayProcessor` nodes
  before re-wiring. Previously, each call removed connections but left old delay nodes orphaned
  in the graph — unbounded growth on every bypass toggle, plugin move, device change, or chain edit.
- IDs are collected first, then nodes removed in a second pass, because mutating
  `graph.getNodes()` (a `ReferenceCountedArray`) during iteration is unsafe. `removeNode()`
  also disconnects the node automatically.

#### Lane Vector Invariant Across All Mutation Sites (`Source/PreferencesWindow.cpp`)

- Fixed lane-not-saved bug when adding a plugin via the "+ Add Plugin" menu: the callback now
  calls `syncBypassedSize()` after `items.push_back()`, extending both `bypassed` and `lanes`
  to match. Previously `lanes` was left undersized, so the new plugin's lane assignment was
  silently dropped on Apply.

#### Defensive Threading Assertion (`Source/IconMenu.cpp`)

- Added `JUCE_ASSERT_MESSAGE_THREAD` at the top of the device-change branch in
  `changeListenerCallback()`. JUCE's `ChangeBroadcaster` marshals callbacks to the message thread,
  so `reconnectGraph()` is safe to call there — the assertion makes the invariant explicit and
  catches any future regression in debug builds.

#### `DelayProcessor` Allocation Safety (`Source/DelayProcessor.hpp`)

- Clamp `delayInSamples` in `prepareToPlay()` to at most 10 seconds at the current sample rate
  before sizing the circular buffer. Guards against a misbehaving plugin reporting absurd
  latency that would otherwise trigger a gigabyte-sized allocation.
- Added `using juce::AudioProcessor::processBlock;` to expose the base-class double-precision
  overload (silences `-Woverloaded-virtual` from the float-only override).

### Stability — Crash Hardening Around Preferences, Apply, and Plugin Editors

#### Runtime Logging for Silent Failures (`Source/HostStartup.cpp`)

- Added a `juce::FileLogger` at application startup via `juce::FileLogger::createDateStampedLogger()`
  and registered it with `juce::Logger::setCurrentLogger()`.
- Startup and shutdown now emit explicit lifecycle log entries, giving silent exits a persistent
  breadcrumb trail instead of disappearing with no diagnostics.

#### Safer Background Plugin Loading (`Source/IconMenu.cpp`, `Source/IconMenu.hpp`)

- Added `cancelPluginLoading()` to stop and discard in-flight plugin loads before destructive graph
  mutations such as plugin deletion or structural chain changes.
- Plugin state restoration was moved off the worker thread and onto the message thread inside
  `onPluginInstanceReady(...)`. This avoids calling `setStateInformation()` on fragile third-party
  plugins from the background loader thread.
- Added log entries for plugin load begin/end, plugin load failure, plugin instance readiness,
  plugin delete/edit actions, device-change handling, and cancelled loads.

#### Apply-Path Risk Reduction (`Source/IconMenu.cpp`, `Source/PreferencesWindow.cpp`)

- `IconMenu::applyPluginChain()` now compares the requested chain/bypass state to the currently
  persisted chain and returns early when Apply was pressed with no actual plugin-chain change.
  This avoids unnecessary `savePluginStates()`, graph rewiring, and plugin churn during
  device-only preference edits.
- Structural chain changes now cancel active background loads before node deletion/reload begins.
- `PreferencesContentComponent::commitAllSettings()` now validates selected input/output device
  names against the currently active device type before applying them.
- Errors returned by `AudioDeviceManager::setAudioDeviceSetup()` are now logged.

#### Guard Rails Around Fragile Plugin Operations (`Source/IconMenu.cpp`, `Source/PluginWindow.cpp`)

- Added bounds checks to tray-menu handlers for delete, bypass, edit, and move actions so stale
  menu indices cannot walk past the current plugin list.
- Added exception guards around plugin state save/restore so host-side persistence code no longer
  assumes third-party plugin implementations behave safely.
- Added exception guards around plugin editor creation in `PluginWindow::getWindowFor()`, logging
  editor-construction failures instead of trusting plugin UI creation blindly.
- `openPluginEditorFor()` now logs and exits cleanly when a plugin node ID exists in settings but
  the processor node is not actually loaded in the graph yet.

#### Native Debug Verification (`CMakeLists.txt`, `CMakePresets.json`)

- Verified the crash-hardening changes on the native WSL/Linux path using:
  - `cmake --preset ninja-debug`
  - `cmake --build build/ninja-debug -j2`
- This also validated the earlier `LANGUAGES C CXX` fix by compiling JUCE’s bundled
  `juce_graphics_Sheenbidi.c` during the real build.

---

## [3.8.1] — 2026-04-17

### Feature — Run at Startup + Version Label + Right-Click Delete in Chain

#### Version Label in Preferences Window (`Source/PreferencesWindow.cpp`)

- Added `versionLabel` (`juce::Label`) to the bottom-left of the Preferences window,
  displaying `v{JUCE_APPLICATION_VERSION_STRING}` in small (11pt) dimmed text (45% alpha).
  Positioned in the same horizontal row as the Apply button so it takes no extra vertical space.

#### Right-Click to Delete from Audio Chain (`Source/PreferencesWindow.cpp`)

- `AudioChainListComponent::mouseDown()` now checks `e.mods.isRightButtonDown()` before the
  existing left-click logic. A right-click on any plugin row shows a `PopupMenu` with a single
  "Delete" item. Selecting it removes the plugin from `items` and `bypassed`, calls `onChange`,
  and repaints. A `juce::Component::SafePointer<AudioChainListComponent>` guards the async
  menu callback against component destruction.

#### Run at Startup Toggle (`Source/IconMenu.cpp`, `Source/IconMenu.hpp`)

- Three new Windows-only private methods on `IconMenu`:
  - `getStartupShortcutPath()` (static) — calls `SHGetKnownFolderPath(FOLDERID_Startup, ...)` to
    locate the current user's startup folder and returns the path for `Light Host.lnk` within it.
    Uses `CoTaskMemFree` to release the path buffer. Returns an empty `juce::File` on failure.
  - `isStartupEnabled() const` — returns `getStartupShortcutPath().existsAsFile()`. Called at
    menu build time to drive the tick mark.
  - `setStartupEnabled(bool)` — if `false`, calls `shortcut.deleteFile()`. If `true`, creates a
    COM `IShellLink` shortcut via `CoCreateInstance(CLSID_ShellLink)`, sets `SetPath` to the
    running executable, `SetWorkingDirectory` to its parent, `SetDescription` to `"Light Host"`,
    then saves via `IPersistFile::Save`. Creates the destination directory first.
- `timerCallback()`: adds `menu.addItem(5, "Run at Startup", true, isStartupEnabled())` inside
  a `#if JUCE_WINDOWS` guard, between the plugin-chain section and the Quit/Delete items.
- `menuInvocationCallback()`: dispatches id=5 to `im->setStartupEnabled(!im->isStartupEnabled())`
  inside a `#if JUCE_WINDOWS` guard.
- `IconMenu.hpp`: three method declarations added inside `#if JUCE_WINDOWS` guard.
- `IconMenu.cpp`: `#include <shlobj.h>` added inside the existing `#if JUCE_WINDOWS` block.
- No registry access — shortcut only. Removing the shortcut fully disables run-at-startup.

---

## [3.8.0] — 2026-04-17

### Feature — Device API Dropdown + Staged Apply-Only Device Settings

#### Dynamic Mono/Stereo Channel Labels (`Source/PreferencesWindow.cpp`)

- **Problem:** The INPUT and OUTPUT channel labels were hardcoded to "Stereo (Ch 1-2)" even
  when the selected device is mono (e.g. a USB microphone with a single channel).
- **Fix:** New `updateChannelLabels()` method queries `deviceManager.getCurrentAudioDevice()
  ->getInputChannelNames().size()` (capped at 2) immediately after every `rebuildDeviceCombos()`
  call. If the device reports 1 channel the label reads "Mono (Ch 1)"; 2+ channels → "Stereo
  (Ch 1-2)". Called on construction and whenever the device manager fires a change notification.

#### "Scan Custom Folder..." in Options Menu (`Source/IconMenu.cpp`)

- **Problem:** Plugin scanning only covered standard JUCE search paths. Users with plugins in
  non-standard directories had no way to add them without manually editing paths.
- **Implementation:** `PluginListWindow` now nests `AugmentedPluginListComponent`, a subclass of
  `juce::PluginListComponent`. In its constructor the existing `getOptionsButton().onClick` is
  replaced with a lambda that calls `createOptionsMenu()`, appends a separator and "Scan Custom
  Folder..." (ID 9999), then shows the combined menu. When selected, `browseAndScan()` opens a
  `FileChooser` (via `launchAsync`) and passes the chosen directory to `ScanJob`, a nested
  `juce::ThreadWithProgressWindow` that runs `PluginDirectoryScanner` for every registered format
  against the selected path. Discovered plugins are added to `knownPluginList` as usual.
- No new UI elements are added outside the existing Options button popup.

#### Device API Dropdown in Device Settings (`Source/PreferencesWindow.cpp`)

- **Problem:** WASAPI exclusive mode uses the device's native hardware clock. When a USB
  microphone (input) and a USB DAC (output) are separate devices each has its own clock; tiny
  drift accumulates and causes audible crackling. WASAPI shared mode routes audio through the
  Windows Audio Session API which mediates the clocks, eliminating drift.
- **Addition:** New "Device API:" row added to the DEVICE SETTINGS section:
  - `deviceTypeHeadLabel` — dark section header matching the existing style.
  - `deviceTypeCombo` — `ComboBox` populated from `deviceManager.getAvailableDeviceTypes()`.
    Reflects the current API on load and after every device-change notification.
  - `kFixedBelowChain` updated to include the extra row height.
  - `rebuildDeviceCombos()` now also populates `deviceTypeCombo` alongside the existing input,
    output, sample-rate, and buffer-size combos.

#### Preferences Chain Refresh from Tray Menu (`Source/IconMenu.cpp`, `Source/IconMenu.hpp`)

- **Problem:** Deleting, bypassing, or moving a plugin via the right-click tray menu did not
  update the Preferences window if it was open, leaving the chain list stale.
- **Fix:** New private method `refreshPreferencesIfOpen()` builds a fresh chain and bypass
  vector from settings and calls `preferencesWindow->refreshPluginChain(chain, bypass)`.
  Called at the end of `handleDeletePlugin()`, `handleBypassPlugin()`, and `handleMovePlugin()`.
- `PreferencesWindow::refreshPluginChain()` delegates to `PreferencesContentComponent::setChain()`
  which updates `chainList.items`, `chainList.bypassed`, recalculates the list height, and calls
  `repaint()`. `setChain()` is public on `PreferencesContentComponent`.

#### Staged Apply-Only Device Settings (`Source/PreferencesWindow.cpp`)

- **Problem:** Device combos had `onChange` callbacks that called `setAudioDeviceSetup()` and
  `setCurrentAudioDeviceType()` immediately on selection. Rapid changes (e.g. device type then
  sample rate in quick succession) triggered overlapping device restarts that could crash the app
  silently.
- **Fix:** Removed all `onChange` callbacks from device combos (input, output, sample rate, buffer
  size, device API). All device-change logic consolidated into `commitAllSettings()`, called only
  when the Apply button is clicked. Flow:
  1. Apply button is disabled immediately (visual feedback; prevents double-clicks).
  2. Chain and bypass state from the staged list are captured.
  3. `MessageManager::callAsync` is used so the button visually greys out before the message
     thread blocks on device restart.
  4. Inside the lambda: device type change (if needed), then input/output device names, then
     sample rate and buffer size are applied via a single `setAudioDeviceSetup()` call.
  5. `onApplyFn(chain, bypass)` fires to commit the plugin chain.
  6. Apply button is re-enabled.
  7. `juce::Component::SafePointer<PreferencesContentComponent>` guards all lambda accesses
     against component destruction between scheduling and firing.
- Removed: `suppressComboCbs` flag, all `ScopedValueSetter<bool>` guards, `applyDeviceType()`,
  `applyDeviceChange()`, `applySampleRate()`, `applyBufferSize()` methods.

---

## [3.7.0] — 2026-04-17

### Feature — Redesigned Preferences UI (Signal-Flow Single View)

Replaced the two-tab (Audio / Plugins) Preferences window with a single unified
signal-flow layout that matches how audio actually travels: INPUT → AUDIO CHAIN
→ OUTPUT → DEVICE SETTINGS.

**`Source/PreferencesWindow.h`**

- Updated constructor signature: added `const std::vector<bool>& bypassStates`
  initial bypass states per plugin, updated `onApply` callback to deliver both
  chain and bypass states, added `onEditPlugin` callback for opening a plugin
  editor from within the chain list.

**`Source/PreferencesWindow.cpp`** — full rewrite

- **`SectionLabel`** — new paint-based component that renders a dark header bar
  (INPUT / AUDIO CHAIN / OUTPUT / DEVICE SETTINGS).

- **`AudioChainListComponent`** — replaces the old `ActivePluginListComponent`.
  Each row now shows:
  - Checkbox (filled blue + tick = active; hollow = bypassed) — clicking toggles
    bypass state in the staged list without requiring Apply.
  - Plugin name (dimmed at 38 % opacity when bypassed).
  - [Edit] button — fires `onEditClicked(index)` to bring up the plugin UI.
  - Drag handle (☰, three bars, right edge) — drag anywhere else in the row to
    reorder. Bypass state travels with the plugin during reorder.
  - Blue drop-indicator line previews the insert position.

- **`PreferencesContentComponent`** — replaces the old `PreferencesContentComponent`.
  - Removed `TabbedComponent` and `AudioDeviceSelectorComponent`.
  - INPUT section: ComboBox of all available input device names (from
    `AudioIODeviceType::getDeviceNames(true)`) + static "Stereo (Ch 1-2)" label
    (stereo-only, no channel 3-8 exposure).
  - AUDIO CHAIN section: scrollable `AudioChainListComponent` inside a Viewport
    (height grows dynamically with window height) + "+ Add Plugin" button that
    opens a manufacturer-grouped popup menu.
  - OUTPUT section: ComboBox of all available output device names + static
    "Stereo (Ch 1-2)" label.
  - DEVICE SETTINGS section: Sample Rate ComboBox (e.g. "48 kHz") and Buffer
    Size ComboBox (e.g. "512 samples  (10.7 ms)") — always visible, no
    "Show advanced settings" toggle.
  - Device / sample-rate / buffer-size changes call `setAudioDeviceSetup()`
    immediately (same real-time behaviour as the old `AudioDeviceSelectorComponent`).
  - Registers as a `ChangeListener` on `AudioDeviceManager`; rebuilds all four
    device combos whenever the manager fires a change, keeping the UI in sync
    with changes triggered elsewhere (e.g. auto-match on device swap).
  - `suppressComboCbs` guard (`ScopedValueSetter<bool>`) prevents combo
    `onChange` from re-firing `setAudioDeviceSetup` during a programmatic
    combo refresh.
  - Window size: 520 × 560 (was 720 × 520); resize limits 440–900 × 500–1000.

- Removed `AvailablePluginsListComponent` and `PluginsTabComponent` — replaced
  by the "+ Add Plugin" popup menu in `PreferencesContentComponent`.

**`Source/IconMenu.hpp`**

- `applyPluginChain()` now takes a second parameter `const std::vector<bool>& bypassStates`.
- Added `openPluginEditorFor(const PluginDescription&)` private method.

**`Source/IconMenu.cpp`**

- `showPreferences()`: builds initial `bypassStates` vector from persisted
  settings, passes it and the two new callbacks to `PreferencesWindow`.
- `openPluginEditorFor()`: looks up the node ID from settings, finds the graph
  node, and brings the `PluginWindow` to front.
- `applyPluginChain()`: Step 3 now also writes the bypass key for each plugin
  in the new chain from the delivered `bypassStates` vector, so checkbox state
  is persisted and takes effect immediately via the existing `reconnectGraph()`.

---

## [3.6.0] — 2026-04-17

### Feature — Explicit Sample Rate Auto-Match on Device Change

**`Source/IconMenu.hpp`**

- Added `bool isHandlingDeviceChange = false;` member — re-entrant guard to prevent `setAudioDeviceSetup()` from recursively triggering `changeListenerCallback`.
- Added `void autoMatchSampleRate();` private method declaration.

**`Source/IconMenu.cpp`**

- **New method `autoMatchSampleRate()`:** When called after a device change, queries the current device's `getAvailableSampleRates()`. If the active sample rate is already in the supported list, returns immediately (no-op). Otherwise, selects the highest-priority supported rate from the ordered preference list `48000 → 44100 → 96000 → 88200 → 192000 → 32000`, falling back to `available.getLast()` if none match. Calls `deviceManager.setAudioDeviceSetup(setup, true)` to apply.

- **Updated `changeListenerCallback()` — `deviceManager` branch:** Expanded from a bare `reconnectGraph()` call to a full device-change handler:
  1. Re-entrant guard (`isHandlingDeviceChange`) prevents infinite callback loops caused by `setAudioDeviceSetup` re-firing the change notification.
  2. `reconnectGraph()` — re-wires the audio graph for the new device channel layout (mono/stereo).
  3. `autoMatchSampleRate()` — ensures the sample rate is valid for the new device.
  4. Audio device state persisted to settings via `deviceManager.createStateXml()` so the selected device and rate survive app restarts.

- **Behaviour:** Switching audio devices in Preferences now explicitly validates and corrects the sample rate in addition to rewiring the graph. Previously, JUCE's `AudioDeviceManager` would attempt to keep the last-used rate when changing devices; if the new device didn't support that rate, behaviour was undefined. Now the app always ends up with a known-good rate.

### Safety — Re-entrant Guard and Window Lifetime

**`Source/IconMenu.cpp`**

- **`changeListenerCallback()` — `isHandlingDeviceChange` flag:** Replaced manual `flag = true` / `flag = false` pair with `juce::ScopedValueSetter<bool>`. The flag is now guaranteed to clear on scope exit regardless of exceptions, eliminating the risk of permanently silencing future device-change callbacks if a throw occurred inside `reconnectGraph()` or `autoMatchSampleRate()`.

- **`showPreferences()` — `onClose` lambda:** The lambda previously called `im->preferencesWindow.reset()` synchronously, which destroyed the `PreferencesWindow` — and therefore the `std::function` containing the executing lambda — while still on that lambda's call stack. This is undefined behaviour (destroying a callable's internal storage during its own execution). Fixed by wrapping the `reset()` in `juce::MessageManager::callAsync`, deferring destruction until after the current call stack fully unwinds.

---

## [3.5.0] — 2026-04-17

### Feature — Automatic Mono Input → Stereo Duplication

**`Source/IconMenu.cpp`**

- **Problem:** The audio graph was hardcoded to wire input channel 0 → output channel 0 and input channel 1 → output channel 1. If a mono input device (e.g. a microphone providing only 1 channel) was selected, channel 1 did not exist on the input node. The connection for channel 1 silently failed, leaving the right output channel silent.

- **How JUCE handles channel counts:** `deviceManager.initialise(2, 2, ...)` requests up to 2 input channels. If the selected device only has 1 channel, JUCE silently clamps — the graph's audio input node exposes only channel 0. `AudioProcessorGraph::addConnection()` returns `false` when the source channel index is out of range for the source node. This return value was previously ignored.

- **Fix — `reconnectGraph()`:** Three connection sites that wire the input node to its destination (first plugin, output passthrough, all-bypassed passthrough) now use a fallback pattern: attempt the stereo channel-1 connection; if it returns `false` (mono device), connect channel 0 to channel 1 instead (duplicate). No explicit channel-count query needed — the graph self-reports via the connection attempt.

  ```
  graph.addConnection(inputCh0 → destCh0)                   // always succeeds
  if (! graph.addConnection(inputCh1 → destCh1))            // stereo path
      graph.addConnection(inputCh0 → destCh1)               // mono fallback: duplicate
  ```

- **Fix — `changeListenerCallback()`:** Added `else if (changed == &deviceManager) { reconnectGraph(); }`. When the user changes the input device in Preferences, the device manager fires a change notification. The graph is immediately rewired to match the new device's channel configuration (mono or stereo) without requiring an app restart.

- **Fix — constructor/destructor:** Added `deviceManager.addChangeListener(this)` in the constructor (after `addAudioCallback`) and `deviceManager.removeChangeListener(this)` as the first line of the destructor (before `removeAudioCallback`). This ensures the change listener is always properly paired and cleaned up before the audio callback is removed.

- **No change required to `deviceManager.initialise(2, 2, ...)`** — JUCE already clamps to available channels. The graph input node naturally has 1 channel for a mono device and 2 for stereo; the routing logic adapts at connection time.

- **Plugin chain unchanged** — stereo plugins still receive 2 channels (both sourced from channel 0 on mono input). The stereo-only plugin filter (`removePluginsLackingInputOutput`) is intentionally preserved: all plugins in the chain process the full stereo signal regardless of input device channel count.

---

## [3.4.0] — 2026-04-17

### Feature — Unified Right-Click Menu + Direct Left-Click to Preferences

**`Source/IconMenu.cpp` / `Source/IconMenu.hpp`**

- **Before:** Two separate menus. Left-click showed a context menu with Preferences, Edit Plugins, and the active plugin chain. Right-click showed a context menu with Quit and Delete Plugin States.

- **After:**
  - **Left-click** opens Preferences directly — no menu, instant. `mouseDown()` calls `showPreferences()` immediately on a left-click event and returns without starting the timer.
  - **Right-click** shows a single combined menu containing all options:
    1. Preferences → `showPreferences()`
    2. Edit Plugins → `reloadPlugins()` (plugin scanner)
    — separator —
    Active Plugins [section header]
    [plugin submenus: Edit / Bypass / Move Up / Move Down / Delete]
    — separator —
    3. Quit → `savePluginStates()` + `quit()`
    4. Delete Plugin States → `deletePluginStates()` + `loadActivePlugins()`

- **Implementation changes:**
  - `timerCallback()`: removed dual-branch logic (`if (menuIconLeftClicked)`). Now unconditionally builds the combined menu. Quit/Delete Plugin States moved to the bottom with IDs 3 and 4 (were 1 and 2 in the old right-click-only menu, now below the full plugin chain section).
  - `mouseDown()`: removed `menuIconLeftClicked = e.mods.isLeftButtonDown()` and `startTimer(50)` for left clicks. Left click calls `showPreferences()` and returns immediately; right click starts the timer as before.
  - `menuInvocationCallback()`: removed `menuIconLeftClicked` branch split. Now a single linear dispatch: IDs 1–4 for fixed items, plugin action ID ranges for chain operations.
  - `bool menuIconLeftClicked` member removed from `IconMenu.hpp` — no longer needed.

---

## [3.3.0] — 2026-04-17

### Production Readiness Pass — Full Codebase Audit

---

#### Bug Fix — Duplicate Plugin Windows (`Source/PluginWindow.cpp` — `getWindowFor()`)

- **Bug:** For plugins with no native editor (those where `createEditorIfNeeded()` returns `nullptr`), `getWindowFor(node, Normal)` falls back internally and stores the resulting window with `type = Generic`. On a second call with `Normal`, the existing-window search compared against `type == Normal` — the stored window had `type == Generic` and was not matched. This caused a duplicate Generic window to be created for the same plugin node: two `GenericAudioProcessorEditor` instances referencing the same `AudioProcessor`, two windows on screen, two parameter-change listeners registered. Triggered any time the user clicked "Edit" twice on a plugin with no native UI.

- **Fix:** The existing-window loop now also matches `Generic` when the requested type is `Normal` (`w->type == type || (type == Normal && w->type == Generic)`). This correctly returns the already-open fallback window instead of creating a duplicate.

---

#### Dead Code Removal — `handleAddPlugin()` (`Source/IconMenu.hpp`, `Source/IconMenu.cpp`)

- **Context:** `handleAddPlugin()` was the handler for selecting a plugin from the tray left-click menu's "Available Plugins" list. In v3.1.0, the available-plugins list was removed from the tray menu and all plugin management moved to `PreferencesWindow`. The dispatch path in `menuInvocationCallback` that routed to `handleAddPlugin()` was deleted at that time, but the function declaration (in `IconMenu.hpp`) and full implementation (in `IconMenu.cpp`, ~50 lines) were left behind as dead code — never called from any path.

- **Fix:** Removed the declaration from `IconMenu.hpp` and the implementation from `IconMenu.cpp`.

---

#### Cleanup — Redundant `onChange` Assignment in `syncLists()` (`Source/PreferencesWindow.cpp`)

- **Issue:** `PluginsTabComponent::syncLists()` re-assigned `activeList.onChange` to the same `[this] { onActiveListChanged(); }` lambda already set unconditionally in the constructor. The re-assignment was a no-op on every call.

- **Fix:** Removed the redundant assignment from `syncLists()`.

---

## [3.2.0] — 2026-04-17

### Bug Fix — Crash on System Shutdown / Restart

**`Source/IconMenu.cpp` — `IconMenu::~IconMenu()`**

- **Root cause:** `IconMenu::~IconMenu()` never called `deviceManager.removeAudioCallback(&player)` before returning. Because `deviceManager` is declared before `player` and `graph` in `IconMenu.hpp`, C++ destroys members in reverse declaration order — meaning `player` and `graph` are destroyed while `deviceManager` still holds the audio device open and is actively firing `audioDeviceIOCallback()` on the real-time audio thread. This is a guaranteed use-after-free: the audio thread writes into a partially-destroyed `AudioProcessorPlayer` and `AudioProcessorGraph`.

- **Why it crashed specifically on restart/shutdown:** On normal tray-menu quit, the JUCE message loop is healthy and the race window is narrow (sometimes survived). On Windows restart/shutdown, the OS disrupts audio services in parallel with the application's shutdown sequence — widening the race window and making the use-after-free reliably fatal.

- **Fix:** Added `deviceManager.removeAudioCallback(&player)` and `player.setProcessor(nullptr)` as the first two operations in `IconMenu::~IconMenu()`, before the plugin load thread is stopped and before any other cleanup. `removeAudioCallback` synchronously waits for any in-progress audio callback to complete (JUCE holds a lock), then removes the callback registration. After it returns, the real-time thread will never touch `player` or `graph` again, making their subsequent destruction safe.

---

## [3.1.0] — 2026-04-16

### Feature — Preferences Window (Plugin Management + Audio Settings)

**New files: `Source/PreferencesWindow.h` / `Source/PreferencesWindow.cpp`**

- **Problem:** Plugin discovery (Available Plugins) and active-chain management lived inside the tray left-click popup menu — a flat, ephemeral context menu with no room for a visual chain overview, no drag-to-reorder, and no safe staging area before changes hit the live audio graph.

- **Design:** A persistent `DocumentWindow` ("Preferences") with two tabs:
  - **Audio tab** — `AudioDeviceSelectorComponent` wired to the existing `AudioDeviceManager`; device changes take effect in real-time as before; state is persisted to settings when the window closes.
  - **Plugins tab** — split-panel layout:
    - **Left: Available Plugins** — `ListBox` (`AvailablePluginsListComponent`) showing all scanned plugins sorted by manufacturer then name, with manufacturer section headers. Greyed-out items are already in the staged chain. Single-click adds a plugin to the staged chain.
    - **Right: Active Plugins** — custom `ActivePluginListComponent` in a `Viewport`. Each row shows a drag handle, plugin name, format badge, and an X delete button. Drag any row to reorder; click X to remove. Scrolls when the chain exceeds the panel height.
    - **Apply button** (bottom-right): commits the staged chain via `applyPluginChain()` and keeps the window open for further editing.
  - Closing the window without pressing Apply silently discards staged changes.

- **`Source/IconMenu.cpp` — `showPreferences()` (replaces `showAudioSettings()`)**
  - Opens `PreferencesWindow` or brings it to front if already open.
  - Passes `SafePointer<IconMenu>` in both the `onApply` and `onClose` lambdas so neither fires on a destroyed `IconMenu`.
  - `onClose` saves audio device XML to settings and calls `preferencesWindow.reset()`.

- **`Source/IconMenu.cpp` — `applyPluginChain()`**
  - Accepts the full new ordered `vector<PluginDescription>` from the Preferences window.
  - Step 1: removes plugins absent from the new chain (closes UI windows, removes graph nodes, removes all settings keys, removes from `activePluginList`).
  - Step 2: assigns stable NodeIDs and adds new plugins to `activePluginList`.
  - Step 3: writes sequential time-based `order` values for the full new chain so `getTimeSortedList()` reflects the exact drag order.
  - Step 4: if any new plugins were added → `loadActivePlugins()` (background thread, full reload); reorder/remove-only changes → `reconnectGraph()` only (zero DLL loads).

- **`Source/IconMenu.cpp` — `timerCallback()`**
  - Removed the "Available Plugins" section (separator + section header + `KnownPluginList::addToMenu`) from the left-click branch. All other menu items are unchanged.
  - Removed the dead `KnownPluginList::getIndexChosenByMenu` dispatch branch from `menuInvocationCallback` (no available-plugin IDs exist in the menu anymore).
  - Menu item 1 now routes to `showPreferences()` instead of `showAudioSettings()`.

- **`Source/IconMenu.hpp`**
  - Forward-declared `class PreferencesWindow`.
  - Added `std::unique_ptr<PreferencesWindow> preferencesWindow` member.
  - Replaced `showAudioSettings()` declaration with `showPreferences()` and `applyPluginChain()`.

- **`CMakeLists.txt`** — added `Source/PreferencesWindow.cpp` to `target_sources`.

---

## [3.0.0] — 2026-04-16

**Release build:** VS 2026 Community (MSVC 19.50), CMake 4.3.1, Windows SDK 10.0.26100.0, x64, Release (/O2 /GL /LTCG)

Consolidates all optimization and performance work from sessions 1–5 into a stable release baseline. No breaking changes to settings format or plugin state persistence. All v2.x settings files load cleanly.

See 2.x entries below for full change details.

---

## [2.4.0] — 2026-04-16

### Performance — Background Plugin Loading on Startup

**`Source/IconMenu.hpp` / `Source/IconMenu.cpp` — `PluginLoadThread` + async `loadActivePlugins()`**

- **Problem:** `loadActivePlugins()` called `createPluginInstance()` for every active plugin sequentially on the message thread. Each call loads a DLL and runs the plugin's constructor — for N plugins this blocked the UI for N × (per-plugin load time). The app was unresponsive until all plugins finished loading.

- **Fix:** Introduced `PluginLoadThread` (a `juce::Thread` subclass, nested in `IconMenu.cpp`). `loadActivePlugins()` now:
  1. Assigns all NodeIDs and reads all settings on the message thread (settings writes must stay on the message thread)
  2. Wires audio input → output immediately so audio passes through while plugins load
  3. Sets tray tooltip to "Light Host - loading..."
  4. Starts `PluginLoadThread` which calls `createPluginInstance()` + `setStateInformation()` for each plugin on the worker thread
  5. For each completed instance, posts it back to the message thread via `MessageManager::callAsync`
  6. The message thread adds each node to the graph as it arrives
  7. On completion, the final `callAsync` calls `reconnectGraph()` and restores the tooltip

- **Thread safety:**
  - All `AudioProcessorGraph` mutations (`addNode`, `removeNode`, `addConnection`) remain on the message thread
  - `setStateInformation()` runs on the background thread before the processor is added to any graph (safe — no audio thread accessing it yet)
  - `Component::SafePointer<IconMenu>` used in all `callAsync` lambdas so they become no-ops if `IconMenu` is destroyed before firing
  - Generation counter (`pluginLoadGeneration`) invalidates stale results if `loadActivePlugins()` is called again while a load is in progress
  - `onPluginInstanceReady` cross-checks `activePluginList` before adding to graph, discarding instances for plugins deleted during loading
  - Destructor calls `stopThread(3000)` before `savePluginStates()`, preventing concurrent graph access on teardown

- **Result:** App tray icon appears immediately on startup. Plugins load in the background one at a time; audio passes through (passthrough) until the chain is fully wired.

---

## [2.3.0] — 2026-04-16

### Performance — Second-Pass Optimizations

**`Source/IconMenu.cpp` — `getTimeSortedList()`: O(N log N) settings reads → O(N)**
- **Problem:** The sort comparator called `settings->getValue(getKey(...))` on both arguments for every comparison — O(N log N) string allocations, settings hash lookups, and string-to-int parses for N plugins.
- **Fix:** Pre-read all order values into a `vector<pair<int, PluginDescription>>` before sorting (O(N) reads). The sort comparator now does O(1) integer comparisons only. Strings are allocated once per plugin per sort, not once per comparison pair.

**`Source/IconMenu.cpp` — `handleMovePlugin()`: O(N) → O(1) settings writes, `savePluginStates()` removed**
- **Problem 1:** Rewrote all N plugins' order values on every move when only 2 values (target and neighbor) need to swap.
- **Problem 2:** Called `savePluginStates()` before every move, serializing every plugin's audio state from the processor — expensive and unnecessary since a move doesn't change any plugin's parameters.
- **Problem 3:** The loop identified the target by computing `getKey("move", toMove)` on every iteration (O(N) allocations) then comparing strings, when `i == index` is sufficient.
- **Fix:** Compute neighbor index directly, read both order values, swap them. 4 key string allocations and 2 settings writes total per move.

**`Source/IconMenu.cpp` — `handleBypassPlugin()` / `handleDeletePlugin()`: vector copy → const reference**
- Both functions called `const auto timeSorted = getTimeSortedList()`, copying the entire cached `vector<PluginDescription>`. Changed to `const auto&` — zero copy cost, reference into the existing cache.

**`Source/IconMenu.cpp` — `timerCallback()`: settings pointer hoisted outside menu loop**
- `getAppProperties().getUserSettings()` was called once per active plugin inside the menu-building loop for the bypass state lookup. Hoisted to a single call before the loop.

---

## [2.2.0] — 2026-04-16

### Performance — Add/Remove Plugin UI Freeze

**`Source/IconMenu.cpp` — `handleAddPlugin()` (partial reload → single-plugin load)**
- **Bug:** Adding a plugin called `savePluginStates()` + `loadActivePlugins()`, which destroyed and re-instantiated every plugin in the chain from scratch on the message thread. With N plugins loaded, adding one caused N+1 DLL loads + state restores, blocking the UI for several seconds per plugin.
- **Fix:** Removed `loadActivePlugins()` call. Now only the new plugin is instantiated via `createPluginInstance()`, its NodeID is assigned immediately, it is added directly to the graph, and `reconnectGraph()` wires it in. Existing plugin instances are never touched.
- **Impact:** Adding a plugin now costs exactly 1 DLL load instead of N+1. Unresponsiveness is eliminated for chains of any length.

**`Source/IconMenu.cpp` — `handleDeletePlugin()` (full reload → node removal)**
- **Bug:** Deleting a plugin also called `loadActivePlugins()` after cleanup, re-instantiating all N-1 surviving plugins for no reason.
- **Fix:** Removed `loadActivePlugins()` call. Now uses `PluginWindow::closeCurrentlyOpenWindowsFor(nodeId)` to close any open UI, `graph.removeNode(nodeId)` to destroy only the deleted instance, then `reconnectGraph()` to rewire. No other plugin instances are affected.
- **Impact:** Deleting a plugin now costs 0 DLL loads. Instant regardless of chain length.

---

## [2.1.0] — 2026-04-14

**Release build:** VS 2026 Community (MSVC 19.50), CMake 4.3.1, Windows SDK 10.0.26100.0, x64, Release (/O2 /GL /LTCG)
**Artifact:** `LightHost-2.1.0-win-x64.zip` (2.16 MB) — `Light Host.exe`, readme, license, gpl, third-party attribution, changelog

---

## [Session 1] — 2026-04-14

### Performance — High CPU at Idle

**`Source/IconMenu.cpp`**
- `deviceManager.initialise(256, 256, ...)` → `(2, 2, ...)`
  JUCE was allocating and mixing 256-channel buffers on every audio callback even though the graph only ever uses 2 channels (stereo). This caused significant idle CPU burn proportional to buffer size × channel count.

**`Source/IconMenu.cpp` — `showAudioSettings()`**
- `AudioDeviceSelectorComponent(deviceManager, 0, 256, 0, 256, ...)` → `(deviceManager, 0, 2, 0, 2, ...)`
  Matched the channel limit in the settings dialog to the actual stereo constraint, preventing the device manager from being reconfigured back to 256 channels after opening audio preferences.

---

### Performance — Slow Plugin Loading

**`Source/IconMenu.cpp` — `getTimeSortedList()` (new cached implementation)**
- Replaced the old O(n²) `getNextPluginOlderThanTime()` loop (which re-read every plugin's order key from the settings file on every pass) with a single O(n log n) `std::sort` pass that reads each key exactly once.
- Result is now cached in `sortedPluginCache` (member of `IconMenu`). The cache is invalidated (`sortedCacheDirty = true`) only in `loadActivePlugins()` when the list genuinely changes. All subsequent calls within the same operation (menu build, bypass toggle, save, move) return the cached vector at zero cost.
- Removed `getNextPluginOlderThanTime()` entirely.

**`Source/IconMenu.hpp`**
- Added `mutable std::vector<juce::PluginDescription> sortedPluginCache` and `mutable bool sortedCacheDirty = true` cache members.
- Removed `getNextPluginOlderThanTime()` declaration.
- Added `reconnectGraph()` declaration.

**`Source/IconMenu.cpp` — `reconnectGraph()` (new method)**
- Extracted graph connection wiring into a standalone method that only removes and rebuilds connections without destroying or recreating any plugin instances.
- Handles the "all plugins bypassed" edge case explicitly (wires input directly to output).

**`Source/IconMenu.cpp` — `handleBypassPlugin()`**
- Was calling `savePluginStates()` + `loadActivePlugins()`, which destroyed all plugin instances, reloaded them from disk, and restored state — just to change a routing connection. Now calls `reconnectGraph()` only. Bypass toggles are now near-instant regardless of plugin count.

**`Source/IconMenu.cpp` — `timerCallback()`**
- Removed redundant second call to `getNextPluginOlderThanTime()` inside the active-plugin menu loop. The loop already had `timeSorted[i]` from the cached list; the duplicate traversal existed solely to retrieve the plugin name and was dead work.
- Changed `const auto timeSorted` (copy) → `const auto& timeSorted` (const ref to cache) in the menu-building loop.

---

### Bug Fixes

**`Source/IconMenu.cpp` — `handleDeletePlugin()` (state corruption)**
- **Bug:** The original code called `deletePluginStates()` first (which nuked *all* plugin state keys for *all* plugins), then called `savePluginStates()` after removing the target plugin. This meant every delete operation silently wiped the states of all surviving plugins.
- **Fix:** Reordered to: (1) `savePluginStates()` to preserve surviving plugins, (2) remove only the deleted plugin's `order`, `bypass`, and `state` keys individually, (3) remove from `activePluginList`, (4) `loadActivePlugins()`.

**`CMakeLists.txt` — `CMAKE_SYSTEM_VERSION` ordering**
- **Bug:** `CMAKE_SYSTEM_VERSION` was set *after* `project()`. The comment in the file even stated it must be set before `project()` so the Visual Studio generator picks it up as `WindowsTargetPlatformVersion`. The Windows 11 SDK (10.0.26100.0) was never actually applied to the VS solution.
- **Fix:** Moved `set(CMAKE_SYSTEM_VERSION ...)` to before the `project()` call.

---

### Code Quality

**`Source/IconMenu.cpp` — includes**
- Removed `#include <climits>` — only existed for `INT_MAX` which was used solely in the now-deleted `getNextPluginOlderThanTime()`.
- Added `#include <algorithm>` — required for `std::sort` used in the new `getTimeSortedList()`.

**`Source/PluginWindow.h`**
- Removed `juce::ApplicationProperties& getAppProperties();` forward declaration. This function is defined in `HostStartup.cpp` and only ever called from `IconMenu.cpp`. The declaration in `PluginWindow.h` was unused and created a redundant visible declaration whenever both headers were included in the same translation unit.

---

## [Pre-release Audit] — 2026-04-14

### Bug Fixes

**`Source/IconMenu.cpp` — `savePluginStates()`: `break` → `continue` (data loss)**
- **Bug:** When a plugin fails to instantiate at load time, its NodeID slot is absent from the graph. The save loop called `break` on the first `nullptr` node, silently discarding the saved states of every plugin that came after it in the sorted chain.
- **Fix:** Changed `break` to `continue`. Failed-to-load plugin slots are skipped; all remaining live plugins are correctly saved.

**`Source/IconMenu.cpp` / `Source/IconMenu.hpp` — Windows tray menu always spawning at first-click position**
- **Bug:** `cursorX`/`cursorY` members were initialised to 0 and only populated on the very first call to `timerCallback` (guarded by `if (cursorX == 0 || cursorY == 0)`). Every subsequent menu open appeared at the coordinates of the first ever click, not the current cursor position.
- **Fix:** Removed the guard and the member variables entirely. `GetCursorPos` is now called unconditionally each time the menu is shown, using a local `POINT` variable.

### Architectural Fixes

**`Source/IconMenu.cpp` — `loadActivePlugins()`: safe sample rate / block size on first launch**
- `graph.getSampleRate()` and `graph.getBlockSize()` return 0 if the audio device has never opened (fresh install, no saved device state). Passing 0/0 to `createPluginInstance` is undefined for many plugins.
- Fixed with fallback: `sampleRate = graph.getSampleRate() > 0.0 ? ... : 44100.0`, `blockSize = graph.getBlockSize() > 0 ? ... : 512`. The graph re-prepares all nodes via `AudioProcessorPlayer::audioDeviceAboutToStart` when a real device opens, so the fallback values are only used for instantiation.

**`Source/IconMenu.cpp` — stable persistent NodeIDs (eliminates plugin reload on move)**
- Previously NodeIDs were positional (`i + 1`), so any reorder operation required `loadActivePlugins()` — destroying and recreating every plugin instance — just to change the wiring order.
- NodeIDs are now assigned once per plugin on first load and stored in settings as `plugin-nodeid-{name}{version}{format}`. A `nextPluginNodeId` counter in settings ensures uniqueness across sessions.
- `reconnectGraph()` now looks up NodeIDs from settings rather than computing them from sort position, so it correctly wires any order without touching graph nodes.
- `handleMovePlugin()` now calls `sortedCacheDirty = true` + `reconnectGraph()` instead of `loadActivePlugins()`. Reordering is now as fast as bypassing — no plugin instances are destroyed or recreated.
- `handleEditPlugin()`, `savePluginStates()`, and `handleDeletePlugin()` all updated to look up stable NodeIDs from settings.
- `handleDeletePlugin()` now also removes the `nodeid` settings key on delete (cleanup).
- `savePluginStates()` refactored to range-for over the plugin list using stable NodeIDs; removed the old positional index loop and `activePluginList.getNumTypes()` dependency.

---

## [Build] — 2026-04-14

### Build Fixes

**`CMakePresets.json` — removed spurious `CMAKE_BUILD_TYPE` from VS presets**
- Visual Studio is a multi-config generator and ignores `CMAKE_BUILD_TYPE` entirely, producing a CMake warning on every configure run. Removed from both VS configure presets. Configuration is selected at build time via the `configuration` field in buildPresets.

**`CMakePresets.json` / PATH — CMake 3.31.6 shadowing CMake 4.3.1**
- `C:\Anthropic\.cmake\bin\cmake.exe` (3.31.6) appeared before `C:\Program Files\CMake\bin\cmake.exe` (4.3.1) on PATH. CMake 3.31.6 predates VS 2026 and has no named generator entry for it, causing configure to fail.
- CMake 4.3.1 was already installed and correctly lists `Visual Studio 18 2026` as a generator.
- Fixed by prepending `C:\Program Files\CMake\bin` to the user PATH. New terminal sessions will pick up 4.3.1 first.

**Build result (VS 2026 Community, MSVC 19.50.35728, CMake 4.3.1): clean — zero warnings, zero errors.**
Artifact: `build\default\LightHost_artefacts\Debug\Light Host.exe`
