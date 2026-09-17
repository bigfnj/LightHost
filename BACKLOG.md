# Backlog

Open items after the 2026-09-17 work, which cleared the nineteen that were here
and added four audit passes of its own. They are written to be picked up cold:
what is wrong, where, why it matters, and what the fix is.

## Where things live, if you are new or returning

| Question | File |
|---|---|
| What is still wrong, and what should I pick up? | this file |
| Why was X *not* built? | [DECISIONS.md](DECISIONS.md) -- each entry records the trigger that would reverse it, so check before re-proposing |
| What changed, and when? | [CHANGELOG.md](CHANGELOG.md) |
| How do I ship it? | [RELEASING.md](RELEASING.md) |
| What is it and how do I build it? | [README.md](README.md) |

**Before trusting a green test run**, know what the gates are and what each one
does not cover. `ctest` gives you unit, GUI and three smoke tests that launch the
real application. `tools/render-regression.sh` is the only check that proves the
host did not change a sample, and it needs a scanned plugin list -- `--scan`
provides one headlessly. `tools/build-linux-docker.sh` builds and tests for
Linux locally; the `clang-release` preset covers Clang. macOS is CI-only.
`tools/ci-status.sh` reads a run's conclusion, which `gh run watch` does not.

**The one habit worth keeping** from the 5.3.0 work: when you add or change a
check, break the thing it guards and confirm it goes red before believing it.
Five gates in this repo could not fail, and each had been green for months.

Nothing here blocks a release. The highest-severity items are two ways to lose
an unapplied edit in the Preferences window, both of which have been present
since the window existed and neither of which loses anything already committed.

Three of the items cleared today were introduced the same day, by the work that
was meant to fix the thing next to them. That is the argument for auditing a
change rather than only testing it, and it is why the sections below separate
what was traced from what was only reasoned about.

---

## Bugs

### Staged chain edits are discarded without saying so

`Source/PreferencesWindow.cpp`, `setChain`, against `IconMenu::refreshPreferencesIfOpen`

`setChain` overwrites `chainList.items`, `bypassed` and `lanes` wholesale from
the committed chain. It is reached from `refreshPreferencesIfOpen`, which fires
on a plugin re-declaring its latency, on every tray bypass, move and delete, and
on the Apply-abandon path.

So: open Preferences, add three plugins with "+ Add Plugin", then open a plugin's
editor and switch it to linear phase. The latency change triggers an async update,
the panel is refreshed from the committed chain, and all three staged additions
vanish with no message and nothing in the log.

Nothing committed is lost, which is why this is not higher. What is lost is work
the user has done and not yet applied, and the refresh that destroys it exists
for a good reason, so it cannot simply be removed.

**Fix:** reconcile rather than overwrite -- keep staged additions and removals
that do not conflict with the incoming committed chain, and say so in the status
row when something had to be dropped. Alternatively make the window dirty-aware
and prompt. Either is a design decision, not a patch.

### The async row menus act on whatever is now at that index

`Source/PreferencesWindow.cpp`, the right-click delete and lane menus

Both capture `row` by value and re-validate only the upper bound when the menu
closes. If the list is replaced while the menu is open -- the same triggers as
the item above -- "Delete" removes a different plugin than the one that was
right-clicked, and the lane menu assigns a lane to a different plugin. The bounds
checks hold, so there is no crash, only the wrong plugin edited silently.

**Fix:** capture the `PluginDescription` rather than the index, and resolve it
back to a row when the menu closes. If it is no longer present, do nothing.

### A throw from the chain-list XML write is still uncaught

`Source/IconMenu.cpp`, both `activePluginList` mutation sites

Both wrap `removeType` / `addType` in a `try`/`catch` with a settings rollback,
and the change listener that writes the XML runs later on the message thread via
an async update, so a throw from the write itself escapes with no rollback.
5.2.0 documented the gap; 5.3.0 took the "let it terminate" half of the choice in
the listener and left the asymmetry.

In practice neither can realistically throw, which is why this has been low for
two releases running.

**Fix:** move the persist inside the guarded scope, or state in the listener that
a throw there is unrecoverable and let it terminate -- and then delete the
rollback, because a guard that covers half a transaction is worse than one that
covers none of it.

---

## Suspected, not traced

### `status.onChange` can dereference the Preferences window during its destruction

`Source/IconMenu.cpp`, the `status.onChange` wiring

The handler tests `preferencesWindow != nullptr`, and `std::unique_ptr` does not
null its pointer before running the deleter, so that test passes throughout
teardown. Reachable path: the content component's destructor commits a dirty lane
trim, the settings write fails, `status.report` fires `onChange`, and the handler
calls into a panel that is inside its own destructor body.

It survives today because `preferencesWindow` is IconMenu's last-declared member,
so the panel's own members are still alive. That is a coincidence of declaration
order which nothing states and no test pins.

**Fix:** reset the pointer before destroying, or clear `status.onChange` in the
window's destructor. Needs a failing settings write during window close to
reproduce.

### A plugin could drive a self-sustaining rewire loop

`Source/IconMenu.cpp`, `reconnectGraph` against `audioProcessorChanged`

A plugin that re-announces its latency in response to a bypass write would drive
`reconnectGraph` to `setBypassed` to `audioProcessorChanged` to
`triggerAsyncUpdate` and round again, one turn per message-loop iteration. The
`AsyncUpdater` coalesces, so it would rebuild forever rather than blow the stack.

5.3.0 guards `setBypassed` on the value actually changing, which closes the
obvious route. Not reproduced -- it needs a plugin that behaves this way.

### Drag-reorder guards its erase and not its insert

`Source/PreferencesWindow.cpp`, the row drag path

`bypassed.erase` and `lanes.erase` are bounds-checked; the matching `insert` calls
are not. Every mutation site keeps the three vectors in lockstep today, so it is
unreachable. The asymmetry is the trap, not a live defect.

---

## Dead and test-only code

- **`chain::Store::read` and `Store::stage (const Slot&)`**, and therefore
  `Slot::state`, have no production caller -- every shipped path uses the
  per-field readers and stagers. Unlike `Meter::isWatched` and
  `StatusSink::totalReported` they carry no "for tests" annotation, so they read
  as live API.
- **`PluginWindow.cpp`, the trailing `return nullptr`** after the `ui != nullptr`
  branch is unreachable.

---

## Tests that pass for the wrong reason

These are the ones that do not earn their place; the count is not the point, and
two of them inflate it badly. Run the binary for the current figure -- see the
note about quoting assertion counts, further down, which this line used to
contradict two items above itself.

- **256 assertions that a probe did not alter the buffer**
  (`Tests/SignalMeteringTests.cpp`). `Meter::measure` takes a const reference, so
  the property is enforced by the type system. One max-deviation assertion over
  the block would say the same thing in one line -- the pattern to copy is
  `largestDeparture` in `Tests/GainProcessorTests.cpp`, which is where it
  already exists. There is nothing of that shape in `SignalMeteringTests.cpp`
  today, so this is a rewrite, not a deletion.
- **256 assertions on the lane-trim ramp** (`Tests/GainProcessorTests.cpp`) that
  would NOT fail if the unity skip were deleted, because the settled gain is
  exactly 1.0 and multiplying by it changes nothing.
- **`isUnity` is under-tested at its upper boundary** -- 0.0 and ±0.5 against a
  0.001 tolerance, so widening the tolerance to 0.4 still passes. The LOWER side
  is pinned, by "the buffer is skipped on the decibel rule, not a linear one" a
  few tests down: 0.0005 dB must be inside the tolerance, because
  `decibelsToGain (0.0005f)` is 1.0000576 and a linear re-derivation of the rule
  would multiply the buffer there. So the gap is one-sided.
- **The ramp-continuity test cannot see a block boundary.** Not because the loop
  starts at `i = 1` -- that is correct within one block -- but because the
  settling block before the gain change has its return value DISCARDED. The last
  sample of that block is never compared with the first sample of the one that
  is kept, which is the only place the discontinuity it exists to catch could
  appear.
- **A two-element tie-break test is redundant, not inert.** It would FAIL with
  the tie-break removed, not pass: both standard libraries use a stable
  insertion sort at that size, so `{1,second},{1,first}` and
  `{1,first},{1,second}` would come back in opposite orders and the test asserts
  they agree. The reason to consider retiring it is that the three-element
  version beside it covers the same property, which is a weaker reason than "it
  cannot fail" and may not be reason enough.
- **`NodeIdTests`** asserts bounds that the exact-value assertions a few lines up
  already pin.

### And one gap that matters more than any of them

**There is no `Tests/PreferencesWindowTests.cpp`.** The committed-chain callback,
the device-choice recording, the parallel-vector invariant and the press/release
checkbox contract have no assertions anywhere. Three of the four confirmed bugs
fixed in 5.3.0 were in that file, and all three were found by reading rather than
by a failing test.

---

## Hardening

- **`--scan` combined with `-self-test`** writes the plugin list into the
  throwaway self-test folder. Harmless, and one guard would make it an error.
- **The signal view has no scrollbar.** 5.3.0 stopped it silently truncating and
  made it count the rows it cannot show. A `juce::Viewport` would show them --
  and this is less work than it sounds, because `PreferencesWindow.cpp` already
  holds two worked examples: `chainViewport`, a plain `juce::Viewport` wrapping
  the chain list, and `PreferencesPanelViewport`, a subclass that resizes its
  viewed component to the visible width. Either pattern transfers.

---

## Process

- **The README's front-page screenshot shows v4.0.3.** `docs/images/preferences.png`
  predates the lane trims, the signal view and the status row, so the first thing
  a visitor sees is three releases out of date. Regenerating it needs a person at
  a display, which is why it is here and not fixed.
- **Do not quote an assertion count in a comment.** There were four, already
  disagreeing with each other before today: `CMakeLists.txt` and `BACKLOG.md`
  said 1120, `BACKLOG.md` also said 1131, `tools/build-linux-docker.sh` said
  1131. None had been right for weeks. They are gone, and the prose around them
  now says what it means without a number. The figure changes every time anyone
  adds a test; run the binary. This file quoted one two lines above the item
  saying not to, until 2026-09-17.
- **`Resources/icon.png` is still 1.0 MB** after a lossless pass on 2026-09-17
  took it from 1,149,765 to 1,055,725 bytes (8.2%, pixel-identical). It is a
  1024x1024 RGBA PNG used as both `ICON_BIG` and `ICON_SMALL`, and it remains
  **8.7x** the next largest tracked non-vendored file
  (`Source/PreferencesWindow.cpp`, 121,714 bytes). It was 9.4x before the pass,
  and never the 2.5x this entry used to claim.

  Only 8.2% came out because the artwork is a glossy 3D render carrying 277,089
  unique colours across 1,048,576 pixels, which PNG has little to remove. The
  remaining megabyte is not a compression problem, it is a **resolution** one:
  `juceaide` downsamples this file at build time into
  `LightHost_artefacts/JuceLibraryCode/icon.ico`, which holds 16, 32, 48 and 256
  px entries and totals 118,042 bytes. Nothing above 256 px ever reaches the
  binary. A 256x256 source would therefore cost about what that `.ico` already
  does -- call it a tenth of the current file -- and change nothing that is
  displayed anywhere.

  Left open rather than done because it is a question about the master asset,
  not about the build: whether this repo should keep the 1024 px original at
  all, or keep it somewhere that is not the source tree. Whoever owns the
  artwork decides that.

  Measured and rejected on the way: zeroing the RGB under the 228,523 fully
  transparent pixels (21.8% of the image, currently holding 3,068 distinct
  colours that nothing can ever render) saves a further 9,936 bytes, 0.9%. Not
  worth giving up a `magick compare -metric AE` of 0 for.

---

## Standing checks

Not work items, but they have to be re-run when something upstream changes.
Automated where possible, so they are not a list someone has to remember.

- **System-audio loopback capture** is blocked on JUCE exposing it, and
  `tools/update-juce.sh` greps for it on every version bump. As of 5.3.0 that
  check fails loudly when it cannot look, rather than reporting "nothing (as
  expected)" about a directory that is not there.
- **The GUI unit tests do not run on Linux.** `LightHostTests` is a console app
  and JUCE's X11 backend cannot create a window from one -- it dies with
  `BadAtom` before any assertion runs, even under xvfb. The application is a GUI
  app and is unaffected, and the smoke tests open the real Preferences window
  under xvfb on the same runner. `CMakeLists.txt` prints a configure-time notice
  rather than skipping quietly.
- **`DeviceTap`'s ordering guarantee is untestable here.** It needs
  `juce_audio_devices`, which the test target deliberately does not link. A known
  limit rather than a gap to close.
- **Display scaling at 150%, and the tray icon against a light taskbar**, cannot
  be automated. Recorded in [RELEASING.md](RELEASING.md) as pre-release checks.
- **macOS is built only by CI.** Linux and Clang can now be run before a push --
  `tools/build-linux-docker.sh` and the `clang-release` preset -- so macOS is the
  only platform whose first evidence still arrives after the commit. Its Clang is
  close enough to the local one that they rarely disagree, and rarely is not
  never.

---

## How the previous nineteen were cleared

For 5.3.0, on 2026-09-17. Fifteen were defects, which is a much higher proportion
than the twenty-four before them, because that pass had already taken out the
easy declines.

| How | Count | Where it went |
|---|---|---|
| Fixed | 15 | [CHANGELOG.md](CHANGELOG.md), `## [5.3.0]` |
| Fixed and the premise corrected | 2 | see below |
| Turned from a warning into a tool | 1 | `tools/ci-status.sh` |
| Still open | 1 | the uncaught XML write, above |

### Two premises did not survive being checked

**"The hard part is detecting that the user explicitly chose."** The entry assumed
a device panel that cannot distinguish a deliberate device change from an
incidental one. There is no `AudioDeviceSelectorComponent` in this codebase; the
panel is Light Host's own combo boxes, and nothing writes a device until Apply.
The hard part was real but it was a different one -- the combo shows the
substituted device after a fallback, so reading it at Apply records the wrong
name. A flag on the combos' `onChange` is the honest signal.

**"`toString`'s `default:` label suppresses `-Wswitch`."** Only on MSVC. JUCE
already passes `-Wswitch-enum`, which warns even with a default present, so GCC
and Clang would have caught a missing enumerator all along. MSVC warns for
neither form at `/W4`, because C4062 is off unless named. The silent failure the
entry described could only ever have happened on the machine this is developed
on -- which is the worst place for it, and is now fixed with `/w14062`.

---

## What has been verified, and how

A record, not a work list. Kept because these were open questions for a long time
and the answers should not have to be rediscovered.

### Verified 2026-09-17 (5.3.0)

- **Eight `tools/` files were cited by no document, not six.** The two this
  file's own entry missed were `voice-headroom.sh`, referenced only from a
  CHANGELOG bug-fix line, and `Dockerfile.linux-build`, referenced only from a
  sibling script. Seven of the eight did carry a header comment; the eighth,
  `show-capture-levels.ps1`, opened on `$src = @'` and explained nothing. All
  fourteen are now indexed in [`tools/README.md`](tools/README.md), one line
  each, and the missing header has been written.
- **`docs/mockups/level-meters.html` pinned four colours to the source, not
  one.** `--accent` (`kAccent`), `--warm` (`kCaution`), `--hot` (`kHot`) and
  `--good` (the `0xff74d68c` literal in `PreferencesWindow.cpp`). All four still
  match; only `--accent` said where it came from, so three could have drifted
  silently. All four are annotated now and the page is linked from the README.
  Its chrome colours (`--bg`, `--panel`, `--text`) do NOT match the source and
  never did -- they were hand-picked, and that is recorded in the file so nobody
  "fixes" them into a pin that was never there.
- **The device-substitution record survives what erased its predecessor.** With
  the request pointed at devices that do not exist, both roles were named at
  startup and again on relaunch, while the stored `DEVICESETUP` held no device
  names at all -- two empty strings, which the old code reads as "nothing was
  requested" and reports in silence. With the real settings restored it said
  nothing.
- **Nothing in 5.3.0 changed a sample.** The render regression was checked after
  every phase and stayed at `ee915b91…` throughout, including across the lane
  trim's rewrite from two atomics to one.
- **Linux builds and passes on a developer machine**, for the first time in this
  project's history, in `ubuntu:24.04` with the same dependency list CI uses.
- **Warnings are errors and the gate was mutation-tested four ways**: an unused
  local in a header failed both targets, a third enumerator on `WindowFormatType`
  produced C4062 naming it, an unused local in a `.cpp` failed after the switch
  from per-target to per-file scoping, and an unused file-scope static correctly
  did NOT fire, because that is not a `/W4` warning. The last one is recorded so
  nobody later reads it as a gap.
- **The dependency-install step now fails when the list is missing.** Measured
  against the old form, which exits 0 having installed nothing.

### Verified 2026-09-16 (5.2.0)

- **The device-substitution report works on a real fallback**, and does not cry
  wolf when both requested devices are present.
- **The live chain is not byte-reproducible, and the regression gate does not
  pretend otherwise.** Four paced renders of one input: ReaEQ gave one hash 4/4
  times, Salvor gave two, because its inference lands on different block
  boundaries. The gate therefore uses a deterministic reference chain.
- **The confirmation dialog's button order is checked by CI on all three
  platforms**, rather than needing a person at a Linux machine.

### Verified 2026-09-08

- **Real plugins load and run.** Salvor, smartChain, Alt Denoiser and three Elgato
  plugins have all been instantiated through this code, live and through the
  offline render. Bypass, lane moves, quit and relaunch all survive.
- **The settings migration ran on a real settings file.** 7,489,165 bytes of
  base64 plugin state migrated to the vault.
- **Declared latency is honest.** A chain declaring 4512 samples (94 ms) measured
  80-84 ms by envelope cross-correlation.
- **The graph's inter-lane compensation is correct** and the host adds none of its
  own, which is what fixed the double-padding bug from 4.0.3.
