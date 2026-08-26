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

Found by audit, still present. Each is a real fault, not a style preference.

- **`removePluginsLackingInputOutput()` destroys the user's scan results.** It
  permanently purges every scanned plugin with fewer than two input or output
  channels when the plugin list window closes, and it judges on scan-time
  defaults rather than the negotiated bus layout, so usable mono-to-stereo
  plugins and every instrument are wiped. If a "cannot host this" signal is
  wanted, grey the row out at display time instead.
  (`Source/IconMenu.cpp`, called from the plugin list window's destructor.)
- **Plugins with a blank manufacturer never appear in Add Plugin.** The grouping
  loop only flushes its submenu when the manufacturer name is non-empty, so those
  entries are silently dropped. Common for VST2, which is still supported.
- **The lane popup callback writes to an unvalidated row index.** It captures a
  row, and nothing re-checks it before writing, so it can write past the end of
  the vector. (`Source/PreferencesWindow.cpp`, lane combo callback.)
- **The device-change guard cannot stop the recursion that matters.** The
  `ScopedValueSetter` on `isHandlingDeviceChange` guards against a synchronous
  re-entry that cannot happen, while the real path (a driver that rejects the rate
  it just accepted) can loop unbounded. Needs a real latch.
- **Errors are discarded.** `AudioDeviceManager::initialise`'s error string and
  every `saveIfNeeded()` return value are thrown away, so a failure to persist
  settings is invisible. Wanted: one error sink surfaced in the tray tooltip and
  in Preferences.
- **Plugin delay compensation goes stale.** Nothing listens for
  `audioProcessorChanged`, so a plugin that changes its reported latency (a
  linear-phase EQ switching mode inside its own editor is the usual case) leaves
  the graph compensating for the old value until the next rewire.

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
- **Phase 6: per-lane gain, default unity.** Four lanes carrying the same source
  sum to about +12 dB today with no trim, meter or dry/wet. A gain node plus a
  persisted per-lane value; unity default means no existing user hears a change
  on upgrade. The settings store already has a slot for it.
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
- **Phase 8: version bump, changelog heading, tag, release.** Confirm the release
  workflow produces both artifacts with tests gating publication.

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
