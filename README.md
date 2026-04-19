# Light Host

Light Host is a lightweight desktop audio plugin host built with JUCE. It is designed to live in the system tray on Windows or the menu bar on macOS, with no permanent main window. Audio flows through a realtime `AudioProcessorGraph`, and plugins can be added, removed, reordered, bypassed, and edited from the tray/menu UI and the Preferences window.

## What It Does

- Hosts audio plugins in a simple chained signal path
- Runs as a tray/menu-bar utility instead of a traditional DAW-style app
- Opens a unified Preferences window for:
  - input/output device selection
  - device API selection
  - sample rate and buffer size
  - plugin chain editing
- Supports plugin bypass, reorder, delete, and editor opening
- Saves plugin state, device settings, and plugin order between launches
- Loads active plugins in the background so startup stays responsive

## Supported Plugin Formats

Platform support depends on the build target and available SDK support:

- Windows: VST, VST3
- macOS: VST, VST3, AU
- Linux: VST, VST3

## Key Behavior

- Left-click tray icon: opens Preferences
- Right-click tray icon: opens the action menu
- Plugins are connected in series through a JUCE `AudioProcessorGraph`
- If all plugins are bypassed, input is wired directly to output
- Mono input is duplicated to stereo output where needed

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

- No latency compensation
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
