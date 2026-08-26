# Light Host

Light Host is a lightweight desktop audio plugin host built with **JUCE 9.0.1**. It is designed to live in the system tray on Windows or the menu bar on macOS, with no permanent main window. Audio flows through a realtime `AudioProcessorGraph` with support for parallel processing lanes and automatic delay compensation. Plugins can be added, removed, reordered, bypassed, edited, and assigned to lanes from the tray/menu UI and the Preferences window.

Released version: **4.0.3**. `main` carries unreleased work towards 5.0.0, listed
in [CHANGELOG.md](CHANGELOG.md); what is left before that release is in
[BACKLOG.md](BACKLOG.md).

## What It Does

- Hosts audio plugins in chained and parallel signal paths
- Runs as a tray/menu-bar utility instead of a traditional DAW-style app
- Supports up to four **parallel processing lanes** (Lane 0–3) with automatic
  **Plugin Delay Compensation** so all lanes arrive sample-aligned at the output
- Opens a unified Preferences window for:
  - input/output device selection
  - device API selection
  - sample rate and buffer size
  - plugin chain editing
  - per-plugin lane assignment
- Supports plugin bypass, reorder, delete, and editor opening
- Saves plugin state, device settings, plugin order, and lane assignments
  between launches
- Loads active plugins one at a time, keeping the message loop pumping between
  them so startup stays responsive

## Supported Plugin Formats

Platform support depends on the build target and available SDK support:

- Windows: VST, VST3
- macOS: VST, VST3, AU
- Linux: VST, VST3

## Audio Input on Windows (Important)

Light Host processes an **input** device. If you want to run plugins on a
microphone or an instrument plugged into an audio interface, everything works out
of the box: pick that interface as the input and you are done.

Running plugins on **audio coming from other applications** (a browser, a game, a
media player) needs one extra piece. Windows provides no way for an application
to capture another application's playback, and JUCE's WASAPI backend has no
loopback mode, so Light Host cannot do it alone. You need a virtual audio device
that presents a playback endpoint to other apps and a matching capture endpoint
to Light Host.

The usual choice is **VB-CABLE** from VB-Audio:

1. Install [VB-CABLE](https://vb-audio.com/Cable/) (donationware; needs admin
   rights and a reboot).
2. In Windows Sound settings, set **CABLE Input** as the system playback device,
   or set it per-application under Volume Mixer.
3. In Light Host, choose **CABLE Output** as the input device and your real
   speakers or headphones as the output.

Audio then flows: application → CABLE Input → Light Host → your plugins → your
speakers.

Light Host does **not** bundle, download, or install VB-CABLE. It only detects
whether a virtual input is present, and if none is, shows a **Get VB-CABLE**
button in Preferences that opens the vendor's page in your browser. VoiceMeeter
and other virtual audio drivers work equally well.

## Key Behavior

- Left-click tray icon: opens Preferences
- Right-click tray icon: opens the action menu
- Plugins on the same lane are connected in series through a JUCE `AudioProcessorGraph`
- Lanes run in parallel; their outputs sum automatically at the graph's output node
- Editing the chain rewires only what changed and publishes one render sequence,
  so a bypass toggle no longer clicks or drops out
- Shorter lanes are delayed automatically so all lanes remain sample-aligned
  (Plugin Delay Compensation, performed by JUCE's `AudioProcessorGraph`)
- If all plugins are bypassed, input is wired directly to output

## Lanes and PDC

Each plugin row in the Preferences chain list has a **Lane** dropdown (Lane 0–3).
Plugins on the same lane are chained in order; lanes are processed in parallel
and summed at the output. Lane assignments persist between launches.

Plugin Delay Compensation is automatic and is performed by JUCE's
`AudioProcessorGraph`. When the graph builds its render sequence it accumulates
each node's reported `getLatencySamples()` along every path, takes the maximum
across the paths feeding a node, and inserts a delay on the shorter ones. All
lanes therefore reach the summing point sample-aligned, and the host reports the
slowest lane as its own latency.

Bypassed plugins keep their latency, which is the correct behaviour: a plugin
that reports latency is required to produce the same latency when bypassed, so
bypassing one does not shift its lane in time.

Versions up to 4.0.3 also inserted their own delay processor on shorter lanes.
Because that processor delayed audio without reporting the delay, the graph saw a
zero-latency lane and compensated a second time, so lanes ended up misaligned by
the very amount the compensation was meant to remove. That code has been removed.
`Tests/GraphRenderTests.cpp` now renders impulses through a real graph and asserts
alignment, so a regression here fails the build.

## Project Layout

```text
.
├── Source/                 Application source
├── Tests/                  Unit tests (juce::UnitTestRunner)
├── Resources/              Icons and binary resources
├── Utilities/              Helper scripts
├── lib/                    Vendored JUCE + VST2 SDK
├── CMakeLists.txt          Main build definition
├── CMakePresets.json       Build presets
├── CHANGELOG.md            Change history
├── BACKLOG.md              Known bugs and remaining work
└── README.md               This file
```

## Build Requirements

### Windows

- Visual Studio 2026
- Windows SDK `10.0.26100.0`
- CMake `4.2+` (required by the Visual Studio 18 2026 generator)

Recommended presets:

- `default` for VS Debug
- `release` for VS Release

### WSL / Linux / Native Ninja Builds

- GCC or Clang
- Ninja
- CMake `3.28+`
- `pkg-config`
- ALSA/X11/font development packages

Known required Linux packages for native configure/build:

- `libasound2-dev`
- `libfontconfig1-dev`
- `libfreetype6-dev`
- `libxcomposite-dev`
- `libxcursor-dev`
- `libxinerama-dev`
- `libxkbcommon-dev`
- `libxrandr-dev`
- `libxrender-dev`

## Build Presets

Available configure presets:

- `default`
- `release`
- `ninja-debug`
- `ninja-release`
- `ninja-relwithdebinfo`
- `clang-debug`
- `clang-release`
- `mingw-release`

### Examples

Visual Studio:

```bash
cmake --preset default
cmake --build --preset debug
```

Windows release:

```bash
cmake --preset release
cmake --build --preset release
```

Native Ninja debug:

```bash
cmake --preset ninja-debug
cmake --build build/ninja-debug -j2
```

Native Ninja release:

```bash
cmake --preset ninja-release
cmake --build build/ninja-release -j2
```

## Tests

Unit tests are built by default (`-DLIGHTHOST_BUILD_TESTS=OFF` to skip) and run
via CTest. They use `juce::UnitTestRunner`, so there is no third-party test
dependency.

```bash
cmake --build --preset release --target LightHostTests
ctest --test-dir build/release -C Release --output-on-failure
```

Current coverage is parallel lane alignment, rendered through a real
`AudioProcessorGraph` with stub processors that declare latency: passthrough,
a single lane, two lanes of differing latency, four lanes of chained plugins
with latencies that cross block boundaries, equal-latency lanes, and a bypassed
lane. Each asserts that all lanes sum into one impulse on the same sample and
that the host reports the slowest lane as its latency.

The second area is chain wiring, tested as a pure function over node facts with
no graph at all: serial order within a lane, lanes staying parallel, mono sources
duplicating into both destination channels, mono destinations taking one channel
and no more, nodes that cannot carry audio being wired around, lane indices from
a corrupt settings file being clamped rather than honoured, and two invariants
the live rewiring depends on. Every connection must name a channel the node
actually has, and the same chain must always produce the same wiring.

The third area is the chain settings store: identity surviving a plugin update
and a rename while still telling apart two plugins inside one shell file, staged
writes being invisible until commit and undone by rollback, an erase leaving no
field behind, and the migration from the 4.0.3 key format carrying every value
over exactly once.

The fourth area is plugin state restore: a good blob round-trips, an empty blob
reports "nothing saved" rather than failure, a plugin that throws out of
`setStateInformation` reports failure without letting the exception escape, and
the resulting rule (never save a node whose restore failed) leaves the stored
preset intact.

No audio device and no real plugin are needed, so these run identically on every
platform. CI runs them on every push, and the Release workflow runs them before
publishing an artifact.

### Startup smoke test

`Light Host -self-test` runs the real application (real device manager, real
graph, real tray icon, real message loop, real teardown), inspects its own log,
prints `SELF-TEST PASS` or a list of failures, and exits non-zero on any problem.
Settings and log go to a throwaway folder unique to the run, so a self-test can
never read or overwrite your real configuration. The folder is deleted on success
and kept on failure.

Every self-test run starts from a seeded one-plugin chain written in the 4.0.3
settings format, so it covers the one-shot settings migration, a plugin that
cannot be instantiated being reported and skipped rather than stalling the load,
and the rule that a plugin which never loaded keeps its saved state. The seeded
plugin's format matches no registered format, so none of this needs a real plugin
on disk.

CTest registers three of these: a first run, an identical repeat, and one with
`-multi-instance=`. They are labelled `smoke`, so `ctest -L unit` and
`ctest -L smoke` can be run separately.

This is the only automated check that exercises the destructor ordering in
`~IconMenu` that prevents a shutdown crash. It is not a substitute for testing
that by hand: on a machine with no audio device no callback thread ever starts,
so the race the ordering guards against cannot occur. The smoke test proves the
ordering code runs cleanly, not that the race is fixed.

On Linux this needs a display. JUCE does not degrade gracefully without one —
the tray icon path dereferences a null X display and the process crashes — so
CMake registers the smoke tests only when `xvfb-run` is present, and warns when
it is not.

## Output

Typical app output locations:

- Visual Studio Debug: `build/default/LightHost_artefacts/Debug/Light Host`
- Visual Studio Release: `build/release/LightHost_artefacts/Release/Light Host`
- Ninja Debug: `build/ninja-debug/LightHost_artefacts/Debug/Light Host`
- Ninja Release: `build/ninja-release/LightHost_artefacts/Release/Light Host`

On Windows the executable is `Light Host.exe`.

## Logging and Crash Triage

LightHost writes a single rotating log file at `LightHost.log` under the JUCE system log folder:

- Windows: `%APPDATA%\Light Host\LightHost.log` (typically `C:\Users\<you>\AppData\Roaming\Light Host\LightHost.log`)
- macOS: `~/Library/Logs/Light Host/LightHost.log`
- Linux: `~/.config/Light Host/LightHost.log`

The log is trimmed to 256 KB on every launch, so the file cannot grow indefinitely. Each session begins with a `==== Light Host vX.Y.Z starting at <time> ====` banner so individual runs stay visually separable.

The log records:

- app startup/shutdown
- audio configuration at startup and on every device change (driver, input/output device names, active-vs-total channel counts, sample rate, buffer size)
- plugin load begin/end/failure
- Preferences open/close
- Apply operations
- plugin editor open attempts
- exception captures around plugin state save/restore, plugin-list mutation, and audio device switching

If the host crashes without a message, the log should be the first place to check.

## Known Limitations

- Stereo-focused routing only
- No per-lane gain or metering: parallel lanes sum at unity, so four lanes
  carrying the same source arrive about 12 dB hot
- No side-chain routing
- No MIDI routing
- No undo/redo for chain edits
- No preset snapshot system
- No plugin sandboxing or out-of-process isolation

Known bugs, as opposed to missing features, are listed in [BACKLOG.md](BACKLOG.md).

## Stability Notes

Because Light Host runs plugins in-process, a plugin that crashes the host process can still take the application down. v4.0.3 closed out the host-side production-hardening backlog with exception protection around:

- Preferences Apply (`setCurrentAudioDeviceType` / `setAudioDeviceSetup`)
- plugin loading and state restore
- plugin state save (`getStateInformation` and `saveIfNeeded`)
- plugin editor creation (`PluginWindow::getWindowFor`)
- plugin-list mutation (`activePluginList.addType` / `removeType`)

All exception handlers log via `juce::Logger::writeToLog` so failures land in the log rather than silently corrupting state. A truly unstable plugin can still crash the process directly, because in-process hosting cannot fully sandbox plugin code.

### Plugin loading

Plugins are instantiated on the message thread via `createPluginInstanceAsync`,
one at a time, each load starting from the previous one's completion callback.
There is no loader thread: JUCE loads the DLL and calls the plugin factory on the
message thread whichever thread asks, so a worker only slept while the message
thread did the work, and cancelling one meant a blocking join during shutdown.
Cancellation is now a generation counter bump, and a superseded callback drops
its result when it arrives.

### Chain settings survive a plugin update

Each plugin's position, lane, bypass state, node id and saved preset are stored
against an identity built from the plugin's file, its format and its unique id.
Versions up to 4.0.3 built that key from the plugin's name and version instead,
which had three consequences: updating a plugin changed its version and therefore
orphaned everything the user had set for it, the same plugin installed in two
folders shared one set of settings, and two plugins whose name and version
happened to concatenate alike ("EQ" version "8" and "EQ8" with no version) also
shared one set.

Existing settings are migrated to the new keys once, on the first launch of 5.0.0,
and the old keys are removed. Keys left over from plugins no longer in the chain
are swept up at the same time, but only when the chain is not empty: an empty
chain cannot be told apart from a chain whose saved list failed to load, and in
that case those keys are the only surviving record of the settings.

### Saved presets survive a failed restore

If a plugin throws out of `setStateInformation`, it keeps its factory defaults.
Saving that node back would overwrite the user's stored preset with defaults, so
a transient load failure would become permanent data loss. `Source/PluginState.hpp`
reports whether a restore actually happened, nodes whose restore failed are
recorded, and the save path skips them so the stored blob is left intact.
`Tests/PluginStateTests.cpp` pins that contract.

## License

Light Host's own source code is GPLv2-or-later, inherited from Rolando Islas's
original Light Host.

**The application as a whole is conveyed under AGPLv3.** Light Host links JUCE,
which is AGPLv3 unless you hold a commercial JUCE licence, and this project does
not. It also uses the Steinberg ASIO SDK under its GPLv3 option. GPLv2-or-later
can be taken up to GPLv3, and GPLv3 and AGPLv3 code may be combined, so the
result is distributable, but anyone receiving a Light Host binary receives
AGPLv3 terms.

One exception: the Steinberg VST 2.4 SDK headers in `lib/vstsdk2.4` are governed
by Steinberg's own SDK licensing agreement, which is not GPL-compatible and is
not covered by the grant above. VST2 hosting is on by default
(`JUCE_PLUGINHOST_VST=1`); building with it off and removing that directory
gives a tree without the exception.

See:

- `license` — the full picture, including the VST2 exception
- `agpl-3.0.txt` — AGPLv3, the licence the built application is conveyed under
- `gpl.txt` — GPLv2, for Light Host's own source
- `third_party` — every bundled component and its licence
