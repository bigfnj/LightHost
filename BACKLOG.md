# Backlog — Light Host

State as of 2026-08-26. Everything below is what remains of the 5.0.0 release
plan; what has already landed is in [CHANGELOG.md](CHANGELOG.md) under
`[Unreleased]`.

## Where this stands

`main` carries the 5.0.0 work but `CMakeLists.txt` still says `VERSION 4.0.3` and
there is no 5.0.0 tag. The published release is still v4.0.3. The version bump,
the changelog heading and the tag are the last step, deliberately, because the
release workflow fires on a tag push and would otherwise publish work that has
never been run against a real plugin.

### Not yet verified

- **No real plugin has ever been loaded through this code.** The tests use stub
  processors and the self-test uses a deliberately unloadable plugin. Before
  tagging: load a chain of real VST3s, toggle bypass, move a plugin between
  lanes, quit, relaunch, and confirm order, lanes, bypass and presets survived.
- **The settings migration has only been exercised on synthetic data** (unit
  tests and the seeded self-test). It runs once, on first launch, against the
  real settings file. Back that file up before the first run:
  `%APPDATA%\Light Host\Light Host.settings` on Windows.
- **Windows only.** Linux and macOS have never been built locally; CI is the only
  evidence for those.
- **The JUCE 9 manual checklist was never run.** 9.0.0 rewrote the SVG parser and
  reworked the software renderer. Check the tray icon, the right-click menu,
  Preferences and two or three plugin editors at 100% and 150% DPI, and in both a
  light and a dark taskbar.

## Bugs

None known as of 2026-08-26. Everything the audits turned up has been fixed and is
described in `CHANGELOG.md` under `[Unreleased]`, which is the place to look before
concluding a fault is new.

Two things worth knowing when reading an older audit of this project, because both
have come up more than once:

- Analyses of the **original** Light Host do not apply here. That codebase is
  JUCE 4.2.4 with a Projucer `.jucer`, a VS2015 exporter, and gitignored `lib/`,
  `Builds/` and `JuceLibraryCode/`. This fork is CMake with JUCE 9.0.1 vendored and
  committed, the VST2 SDK at `lib/vstsdk2.4`, and ASIO headers that ship with JUCE
  rather than being a separate download. Line numbers in such an audit will point
  at code that no longer exists.
- The settings key **does** still collide for two instances of the same plugin,
  which is a real observation. It is unreachable, because the chain lives in a
  `juce::KnownPluginList` and that container refuses a duplicate on the same field
  set the key is built from. Making it reachable and making it correct are the same
  piece of work: see the allocated slot ids under Deferred.

## Features and refactors

Phase numbers refer to the 5.0.0 plan.

- **Phase 5 remainder: extract `AudioEngine`.** Split `deviceManager`, `player`
  and `graph` out of `IconMenu`. Read `~IconMenu` first: the shutdown
  use-after-free fixed in v3.2.0 depends on member *declaration* order in that
  class plus statement order in its destructor, and splitting those members
  destroys the invariant with no compiler warning and no possible unit test. The
  detach sequence must move wholesale into `AudioEngine`'s destructor as its
  first statements, comment kept verbatim, and the smoke test should be run under
  a sanitiser as part of the gate.

  **Weigh this before starting it.** It is the only remaining item with no
  user-visible payoff, it carries the worst failure mode in the project (a crash
  on every shutdown, on a path no automated test can reproduce: with no audio
  device there is no callback thread, so the race cannot occur on CI), and the
  sanitiser gate that would justify it has not been set up. The argument for
  doing it is that the invariant currently lives in a 1400-line class and would
  end up in a 60-line one whose whole purpose is to hold it. The argument against
  is that nothing else in the plan depends on it.
- **Phase 6: metering.** The lane trims landed, so lanes can be balanced, but
  there is nothing to look at while doing it. A peak meter per lane, and one on the
  output, would make the trims usable without guessing. This is the remaining half
  of "no trim, meter or dry/wet".
- **Phase 6: dry/wet per lane.** A lane is either in or out. A blend against the
  unprocessed input is what parallel processing is usually for, and the trim nodes
  are the obvious place to hang it.
- **Phase 6: test `PluginWindow.cpp`.** At ~200 lines with one stub processor it
  is the cheapest file in the project to cover, and it holds three historical
  fixes (duplicate Generic windows, a throwing editor constructor, a deprecated
  editor-creation call).
- **Phase 7: stop rewriting every plugin's state on every edit.** Plugin state
  lives base64-encoded inline in one `PropertiesFile`, and each `setValue` plus
  `saveIfNeeded()` rewrites the whole XML document on the message thread, so one
  bypass toggle rewrites every plugin's state blob. Move state blobs to one file
  per slot under a `state/` subdirectory and keep the properties file small. The
  write itself is atomic via `TemporaryFile`, so this is latency, not corruption.
- **Phase 7: `getTimeSortedList()` hands out a reference to a mutable cache.**
  Callers hold it across a rebuild that clears it. The tray-menu handlers copy
  defensively now, which is a patch and not a fix; return a snapshot
  (`shared_ptr<const std::vector<...>>`) instead.
- **Phase 8: version bump, changelog heading, tag, release.** The procedure is
  written down in [RELEASING.md](RELEASING.md). The pipeline itself is ready:
  three platforms, tests gating publication, licence files in every archive,
  checksums, and prerelease tags producing drafts. It has never run end to end,
  so budget for the first candidate exposing something.
- **Code signing, if macOS or Windows downloads are to be first-class.** macOS
  refuses an unsigned app until the quarantine attribute is cleared by hand, and
  Windows shows a SmartScreen warning on every new binary. Certificates cost money
  and are worth it only if the download counts justify them.

## Deferred

- **Allocated slot ids.** The chain settings store derives identity from the
  plugin's file, format and unique ids. That survives an update and a rename but
  still moves if the user relocates the plugin file. A monotonically allocated
  id would survive that too, but it requires the chain to stop being a
  `juce::KnownPluginList` and become the store's own container, which is a much
  larger change than the faults it would fix. Revisit if users report losing
  settings after moving their plugin folder.
- **Duplicate plugins in one chain.** `KnownPluginList` holds unique types, so the
  same plugin cannot appear twice. Would need the same container change as above.
- **Regression tests for the four index-guard clauses and node id stability.**
  These live inside `IconMenu` methods that need a device manager and a tray icon
  to construct, so they need a seam before they can be tested. Not worth forcing
  one on their own; they come for free with the `AudioEngine` split.
- **Out-of-process plugin hosting.** Light Host runs plugins in-process, so a
  plugin that crashes takes the host with it. Real sandboxing is a different
  application, not a fix.
- **System-audio capture: re-check this on every JUCE bump.** Processing audio
  from other applications currently needs a third-party virtual input device
  (VB-CABLE or similar), and the Preferences window says so. That is a JUCE
  limitation and not a Windows one, which is why it is worth re-checking rather
  than treating as settled: WASAPI has supported loopback since Vista
  (`AUDCLNT_STREAMFLAGS_LOOPBACK` on a render endpoint) and Windows 10 2004
  added per-process loopback, but JUCE exposes neither. `WASAPIDeviceMode` is
  `shared`, `exclusive` and `sharedLowLatency`, and `loopback` appears nowhere
  in `juce_audio_devices` as of the vendored 9.0.1.

  The check, after any change to `lib/juce`, is
  `grep -ri loopback lib/juce/modules/juce_audio_devices`. A non-empty result
  means a supported fix has become available, and both the hint text in
  `PreferencesWindow.cpp` and this entry should be revisited.

  Deliberately not worth hand-rolling in the meantime. It needs a fourth WASAPI
  device mode, enumeration that presents render endpoints as inputs, and two
  problems that are the actual work: a loopback capture and a render stream on
  different endpoints are separate clock domains that drift, so it needs
  continuous resampling; and a loopback stream delivers no packets at all while
  nothing is playing to that endpoint, so the graph has to be fed synthesised
  silence or it starves. Wiring stays as it is until JUCE does the work.
