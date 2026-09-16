# Backlog

Open items found by two audit passes over the 5.2.0 tree on 2026-09-16, after the
release work had emptied the previous 24. They are written to be picked up cold:
what is wrong, where, why it matters, and what the fix is.

Nothing here blocks a release. The highest-severity item is a gap in a feature
that shipped in 5.2.0 working — it reports a substituted audio device correctly,
and can lose the ability to notice one later.

---

## Bugs

### The device-substitution report can lose the evidence it depends on

`Source/DevicePolicy.hpp`, `IconMenu::reportDeviceSubstitutionIfAny`
(`Source/IconMenu.cpp:1248`)

The check compares the persisted `audioDeviceState` against the live setup, which
works because JUCE's fallback path does not rewrite the stored request. But
`autoMatchSampleRate`, Preferences Apply, and **any** change made in the device
panel — including something as incidental as a buffer size — call
`setAudioDeviceSetup` with `treatAsChosenDevice = true`, which calls
`updateXml()` and adopts the *fallback* device as the stored choice.

Once that happens the requested name is gone. The comparison then finds nothing
wrong, so a substitution that is still in force becomes undetectable, and a
device that was silently replaced looks correct forever after. 5.2.0 added the
missing check before the third write site and documented the limit; it did not
close it.

**Fix:** record the requested device names in a settings key of our own, written
when the user explicitly chooses a device rather than whenever JUCE decides to,
and compare against that instead of against `DEVICESETUP`. The hard part is
detecting "the user explicitly chose", because the device panel does not
distinguish a deliberate device change from an incidental one. Storing an
endpoint id is declined for separate reasons — see `DECISIONS.md`.

### Signal-view rows can be attributed to the wrong plugin

`Source/PreferencesWindow.cpp:1403` against `IconMenu::reconnectGraph`
(`Source/IconMenu.cpp:1126`)

`refreshSignalView` labels row `i` from `chainList.items[i]`, which is the
**staged** chain, and fills it from `probeMeterAtFn(i)`, which resolves to a probe
indexed over the **committed** chain. The two agree when the view is refreshed
through `refreshPluginChain`, which sets `items` from the committed chain first.
They do not agree on the other path: opening the view calls `refreshSignalView`
directly.

Reorder or delete a row in the Preferences list without pressing Apply, then open
the signal view: every row is labelled with one plugin and metered from another.
That is the same mis-attribution a 5.1.0 fix called "worse than showing nothing",
reached by a different route. Diagnostic only — no effect on audio.

**Fix:** label the rows from the same source the meters come from. Add a callback
beside `probeMeterAt` returning the committed chain's names in display order, and
iterate that rather than `chainList.items`, so labels and meters are indexed
identically by construction.

### The bypass checkbox has no hover or pressed state

`Source/PreferencesWindow.cpp:970`, `:1014`

`Control::checkbox` is produced by `controlAt` but no consumer distinguishes it:
`pressedControl` is only ever set to `lane`, `settings` or `none`, and the paint
path asks only about those two. So the checkbox is the one row control with no
visual feedback, though it does get the pointing-hand cursor.

**Fix:** either handle it in the press and paint paths like the other two, or stop
returning it from `controlAt` and say why.

### A throw from the chain-list XML write is uncaught

`Source/IconMenu.cpp:1484`, `:2125`

Both mutation sites wrap `activePluginList.removeType` / `addType` in a
`try`/`catch` with a settings rollback. The comment now says what that does not
cover, which was the 5.2.0 fix — but the gap is still there: the change listener
that writes the XML runs later on the message thread via an async update, so a
throw from the write itself escapes with no rollback. In practice neither can
realistically throw, which is why this is low.

**Fix:** either move the persist inside the guarded scope, or state in the
listener that a throw there is unrecoverable and let it terminate.

### `activePluginList.addType` result is discarded

`Source/IconMenu.cpp:2125`

`false` means "replaced an existing entry rather than adding". The arriving set is
filtered with `Store::identityOf`, which includes `pluginFormatName`, while
`addType` uses `PluginDescription::isDuplicateOf`, which does not. Two
descriptions sharing a file and unique ids under different format names are
therefore two identities and one list entry, leaving settings for an entry that
does not exist. Narrow, and the asymmetry is acknowledged in `identityOf`'s own
comment.

---

## Duplication with a real drift cost

### `"audioDeviceState"` has no single home

`Source/IconMenu.cpp:342` (read), `:1254` (read), `:1330` (write), `:1927` (write)

5.2.0 added the second reader, and its correctness depends on matching the writers
exactly. A drift means `describeSubstitution` compares the open devices against an
absent stored request and reports nothing, forever — a silent regression of the
feature that release is named for.

**Fix:** one header owning the key name, in the style of `Source/NodeIds.hpp`.

### `"pluginList"` and `"pluginListActive"` are string literals in four files

`Source/IconMenu.cpp:370`, `:1299`; `Source/OfflineRender.hpp:156`;
`Source/SelfTest.hpp:110`; `Tests/PluginChainStoreTests.cpp:445`, `:452` — and
`Source/PluginChainStore.hpp:474` reasons about them in a comment without owning
them.

These are the on-disk chain-membership keys. A drift means `--render` reads a key
`IconMenu` does not write, so the offline renderer renders an empty chain and
reports success — and that file exists to produce trustworthy measurements, with a
whole 5.1.0 entry about five ways it used to report success it had not earned.

**Fix:** the same header as above. `DECISIONS.md` explains why these sit outside
`chain::Store`; that argues for a home for the key *names*, not for literals.

### The peak-decay ballistic is written twice, in different units

`Source/SignalMetering.hpp:78` (`kPeakDecayPerBlock = 0.9496f`, per block, audio
thread) and `Source/PreferencesWindow.cpp:60` (`fallDbPerTick = 1.8f`, per tick at
25 Hz, message thread). Both comments claim "about 45 dB per second" and they
agree today.

If they drift, the UI's fall rate diverges from the held peak's and bars either
sag or stick — the exact symptom that made 5.1.0 move the ballistics into `Meter`.

**Fix:** derive one from the other, or assert the equivalence in
`Tests/SignalMeteringTests.cpp`.

### `gain::isUnity` is unused, and disagrees with the code that should use it

`Source/GainProcessor.hpp:52` against `Processor::processBlock`

`isUnity` is a named, unit-tested rule with no production caller. The place that
should use it re-derives a different test on the linear gain instead, and the two
disagree: `isUnity(0.0005f)` is true while `decibelsToGain(0.0005f)` fails
`approximatelyEqual`. The cost of the disagreement is a redundant `applyGain`,
which is inaudible; the cost of leaving it is a rule that looks live and is not.

**Fix:** call it from `processBlock`, or delete it and its tests.

---

## Dead and test-only code

- **`Processor::getGainDb()`** (`Source/GainProcessor.hpp:78`) has no production
  caller; `IconMenu::getLaneGainDb` reads the value back out of the chain store
  instead.
- **`StatusSink::all()` and `clear()`** (`Source/StatusSink.hpp:67`, `:72`) are
  reached only by tests, and `int total` (`:90`) is read only by
  `totalReported()`. So `clear()`'s documented contract — "a cleared sink still
  admits that something happened earlier" — describes behaviour no shipped path
  can reach. Either annotate them as test-only, as `Meter::isWatched` does, or
  remove them.
- **`toString` in `Source/PluginWindow.h`** carries `default: return {};` on a
  switch that already covers both enumerators, which suppresses `-Wswitch`. A
  third window type added later compiles silently and yields the key
  `"uiLastX_"` — the exact silent-orphan failure the enum's own comment says the
  derived keys prevent.
- **Two includes in `Source/IconMenu.hpp`** (`ConfirmPolicy.hpp`,
  `DevicePolicy.hpp`) are used only by the `.cpp`, as is the pre-existing
  `Lanes.hpp`. Move them.

---

## Hardening

- **`Probe::processBlock` and `gain::Processor::processBlock` could be
  `noexcept`.** Everything they call is non-throwing. Note `DeviceTap`'s device
  callback deliberately must **not** be: it forwards into third-party plugin code,
  where `noexcept` would turn a plugin's throw into `std::terminate`.
- **No `-Werror` / `/WX` anywhere.** `-Wunused-parameter` and `/W4` are on across
  three platforms, so a warning lands in a CI log without failing anything.
  Turning it on would make the compiler a gate rather than a commentator; it would
  also need a pass over existing warnings first.
- **`Tests/PluginWindowTests.cpp`**, the last `PluginWindowGui` block, does not
  assert the window registry is empty on the way out — the only one of eight that
  does not. Harmless because it is last, and it is the seam a future appended
  block falls through.
- **`tools/render-regression.sh`** hard-codes the MSVC artefact path
  (`build/release/...` plus `.exe`), so it only runs on Windows. It fails loudly
  rather than silently, so this is a limit rather than a bug.

---

## Process

- **`gh run watch --exit-status` returns 0 for runs that failed.** Observed twice
  on 2026-09-16, on two different failed runs. Anything gating a release on CI
  must read the run's `conclusion` explicitly. A gate that reports success on
  failure is worse than no gate, and this one is exactly the gate that stops a
  repeat of v5.0.1. Recorded in `RELEASING.md`.

---

## How the previous 24 items were cleared

For 5.2.0, on 2026-09-16. Six different dispositions, and which one applied
matters more than the count — only seven of the twenty-four were defects:

| How | Count | Where it went |
|---|---|---|
| Fixed | 7 | [CHANGELOG.md](CHANGELOG.md), `## [5.2.0]` |
| Declined, with reasoning and a reversing trigger | 7 | [DECISIONS.md](DECISIONS.md) |
| Already shipped, or the premise did not hold | 6 | see below |
| Closed by making it testable rather than checking by hand | 2 | `Tests/ConfirmPolicyTests.cpp`, `Tests/PluginWindowTests.cpp` |
| Automated so it re-checks itself | 1 | `tools/update-juce.sh` |
| Moved to the release procedure, because it needs a person | 1 | [RELEASING.md](RELEASING.md) |

### Two were misdiagnosed, which is worth remembering

**"VST2 scanning does not descend into subdirectories" was not a defect.**
`PluginDirectoryScanner` is constructed with `recursive = true` in both scan
paths — JUCE hard-codes it for its own scan, and our custom-folder scan passes
it — and the nine ReaPlugs DLLs were already present in the scanned list, found
from the parent folder one level down, while the stored search path named only
that parent. The entry appears to have been written from looking at the folder
picker rather than from a scan that missed files. Two real defects were found next
to it and fixed instead: failed plugin loads were discarded, and a custom folder
was never stored so it was never re-scanned.

**"A nested dispatch loop runs in the middle of a graph teardown" overstated where
it runs.** The message pump in `PluginWindow::closeAllCurrentlyOpenWindows` runs
*before* `graph.clear()`, so the graph is whole throughout. The real gap was next
to it and is fixed: the chain reload never cancelled its async updater, which a
hosted plugin triggers when it reports a latency change on the way out. The pump
is kept and now documented. Removing it would need a soak across real VST2 and
browser-hosting editors, because whether any plugin needs a message-loop turn
between its editor and its processor being destroyed is not answerable by reading
code.

### Three were stale rather than outstanding

Per-plugin and device metering shipped in 5.1.0. Rewriting every plugin's state on
every edit stopped in 5.0.0, when state moved to a file per plugin. The release
procedure works — three releases have gone through it — and `RELEASING.md` was
rewritten to describe what is actually done rather than a release-candidate
process that is not used.

---

## What has been verified, and how

A record, not a work list. Kept because these were open questions for a long time
and the answers should not have to be rediscovered.

### Verified 2026-09-16 (5.2.0)

- **The device-substitution report works on a real fallback.** The stored request
  was pointed at a device that does not exist and the application launched against
  it: both the input and the output substitution were named in the log and raised
  through the status sink. Both sides move, because JUCE falls back to the default
  device for each — which is what made the original incident send a microphone
  chain to room speakers.
- **It does not cry wolf.** The same build launched against the real settings, with
  both requested devices present, reported nothing.
- **The live chain is not byte-reproducible, and the regression gate does not
  pretend otherwise.** Four paced renders of one input: ReaEQ gave one hash 4/4
  times, Salvor gave two. This happens while the renderer reports a realtime factor
  of 1.000 with no blocks behind, so "kept pace" does not imply reproducible — the
  inference runs on a worker thread and lands on slightly different block
  boundaries. `tools/render-regression.sh` therefore gates on a deterministic
  reference chain, which still exercises everything in the host.
- **Nothing in 5.2.0 changed a sample.** The render regression was checked after
  every phase and stayed identical throughout.
- **The confirmation dialog's button order is now checked by CI on all three
  platforms**, rather than needing a person at a Linux machine to dismiss a dialog
  and see what happened.

### Verified 2026-09-16 (5.1.0)

- **Plugin state is no longer copied three times per load.** It is moved into the
  async creation lambda; a real state measured 4,126,524 bytes, so a five-plugin
  chain was copying tens of megabytes on the message thread at startup.
- **The self-test log poll re-reads only when the file grows**, rather than reading
  and scanning a 256 KB file up to 200 times per run.
- **Metering does not alter the audio.** A fixed input renders byte-identical
  through `--render` before and after every phase of the 5.1.0 work, and a topology
  test asserts that a chain with no probes wires exactly as it did before probes
  existed.
- **Probe insertion works on a live graph.** The startup self-test opens the signal
  view part way through its run, so adding nodes to a running graph and rewiring it
  is exercised on all three platforms in CI.
- **JUCE 9.0.2 is clean for this project.** Its one breaking change removes an API
  this codebase does not use.

### Verified 2026-09-08

- **Real plugins load and run.** Salvor, smartChain, Alt Denoiser and three Elgato
  plugins have all been instantiated through this code, live and through the
  offline render. Bypass, lane moves, quit and relaunch all survive; order, lanes
  and state persist.
- **The settings migration ran on a real settings file.** 7,489,165 bytes of base64
  plugin state migrated to the vault, leaving roughly twelve kilobytes of readable
  XML. Not synthetic data.
- **Declared latency is honest.** A chain declaring 4512 samples (94 ms) measured
  80–84 ms by envelope cross-correlation, agreeing within the method's resolution.
- **The graph's inter-lane compensation is correct** and the host adds none of its
  own, which is what fixed the double-padding bug from 4.0.3.

---

## Standing checks

Not work items, but they have to be re-run when something upstream changes.
Automated where possible, so they are not a list someone has to remember.

- **System-audio loopback capture** is blocked on JUCE exposing it, and
  `tools/update-juce.sh` greps for it on every version bump, so the answer is
  re-checked without anyone deciding to.
- **The GUI unit tests do not run on Linux.** `LightHostTests` is a console app and
  JUCE's X11 backend cannot create a window from one — it dies with `BadAtom` on
  `X_ChangeProperty` before any assertion runs, even with a working display under
  xvfb. The application is a GUI app and is unaffected, and the smoke tests open
  the real Preferences window under xvfb on the same runner, so Linux window
  creation is covered. What is not covered there is the host logic around windows,
  which has no platform component and is checked on Windows and macOS.
  `CMakeLists.txt` prints a configure-time notice rather than skipping quietly.
  Making the test target a GUI app would fix it and would cost stdout on Windows,
  which is where the other 1131 assertions report from.
- **Display scaling at 150%, and the tray icon against a light taskbar**, cannot be
  automated — they need a person looking at a display configured that way.
  Recorded in [RELEASING.md](RELEASING.md) as pre-release checks.
- **Linux and macOS are built by CI on every push**, including the unit, GUI and
  smoke suites. No developer machine here has ever built them, and there is no GCC
  or Clang on the development box, so CI is not merely the best evidence for those
  platforms — it is the only evidence, and it arrives after the push rather than
  before it.

  5.2.0 fell into that gap twice. `expectEquals` was handed a `juce::uint32`; MSVC
  picked an `operator<<` overload and compiled it, and both GCC and Clang
  correctly refused because `juce::String` has overloads for `int`, `int64` and
  `uint64` but not `unsigned int`. Then `unit-gui` failed on X11 as described
  above. A clean local build and 1120 passing tests said nothing about either. The
  tripwire worked as intended — the tag was held until CI was green, which is the
  whole point of that rule — but if MSVC-only divergence happens again, installing
  Clang locally and exercising the `clang-release` preset before pushing is the
  fix.
