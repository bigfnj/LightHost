# Light Host

Light Host is a lightweight desktop audio plugin host built with **JUCE 8.0.13**. It is designed to live in the system tray on Windows or the menu bar on macOS, with no permanent main window. Audio flows through a realtime `AudioProcessorGraph` with support for parallel processing lanes and automatic delay compensation. Plugins can be added, removed, reordered, bypassed, edited, and assigned to lanes from the tray/menu UI and the Preferences window.

Current version: **4.0.1** — see [CHANGELOG.md](CHANGELOG.md).

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
- Loads active plugins in the background so startup stays responsive

## Supported Plugin Formats

Platform support depends on the build target and available SDK support:

- Windows: VST, VST3
- macOS: VST, VST3, AU
- Linux: VST, VST3

## Key Behavior

- Left-click tray icon: opens Preferences
- Right-click tray icon: opens the action menu
- Plugins on the same lane are connected in series through a JUCE `AudioProcessorGraph`
- Lanes run in parallel; their outputs sum automatically at the graph's output node
- An internal `DelayProcessor` is auto-inserted on shorter lanes so all lanes
  remain sample-aligned (Plugin Delay Compensation)
- If all plugins are bypassed, input is wired directly to output
- Mono input is duplicated to stereo output where needed

## Lanes and PDC

Each plugin row in the Preferences chain list has a **Lane** dropdown (Lane 0–3).
Plugins on the same lane are chained in order; lanes are processed in parallel
and summed at the output. Lane assignments persist between launches.

Plugin Delay Compensation is automatic: on every graph reconnect, LightHost
sums each lane's `getLatencySamples()`, finds the maximum across lanes, and
inserts a transparent delay processor on every shorter lane so the summing
point stays phase-aligned. Bypassed plugins contribute zero latency, matching
standard DAW convention.

## Project Layout

```text
.
├── Source/                 Application source
├── Resources/              Icons and binary resources
├── Utilities/              Helper scripts
├── lib/                    Vendored JUCE + VST2 SDK
├── CMakeLists.txt          Main build definition
├── CMakePresets.json       Build presets
├── CHANGELOG.md            Change history
├── AI_UNDERSTANDING.md     Deep machine-readable project notes
└── README.md               This file
```

## Build Requirements

### Windows

- Visual Studio 2026
- Windows SDK `10.0.26100.0`
- CMake `3.28+`

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

## Output

Typical app output locations:

- Visual Studio Debug: `build/default/LightHost_artefacts/Debug/Light Host`
- Visual Studio Release: `build/release/LightHost_artefacts/Release/Light Host`
- Ninja Debug: `build/ninja-debug/LightHost_artefacts/Debug/Light Host`
- Ninja Release: `build/ninja-release/LightHost_artefacts/Release/Light Host`

On Windows the executable is `Light Host.exe`.

## Logging and Crash Triage

The app now creates a date-stamped JUCE file log at startup. This is useful when a plugin or device change causes a silent exit.

The log records events such as:

- app startup/shutdown
- plugin load begin/end/failure
- Preferences open/close
- Apply operations
- plugin editor open attempts
- device-change handling

If the host crashes without a message, the log should be the first place to check.

## Known Limitations

- Stereo-focused routing only
- No side-chain routing
- No MIDI routing
- No undo/redo for chain edits
- No preset snapshot system
- No plugin sandboxing or out-of-process isolation

## Stability Notes

Because Light Host runs plugins in-process, a plugin that crashes the host process can still take the application down. Recent hardening work reduced host-side crash risk around:

- Preferences Apply
- background plugin loading
- plugin state save/restore
- plugin editor creation

That said, a truly unstable plugin can still crash the process directly.

## License

Light Host is distributed under GPLv2+.

See:

- `license`
- `gpl.txt`
- `third_party`
