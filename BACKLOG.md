# Backlog

Open items after the 2026-09-17 work, which cleared the nineteen that were here
and added four audit passes of its own. They are written to be picked up cold:
what is wrong, where, why it matters, and what the fix is.

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

The suite is 1363 headless assertions and 15 GUI. These are the ones that do not
earn their place; the count is not the point, and two of them inflate it badly.

- **256 assertions that a probe did not alter the buffer**
  (`Tests/SignalMeteringTests.cpp`). `Meter::measure` takes a const reference, so
  the property is enforced by the type system. One max-deviation assertion says
  the same thing.
- **256 assertions on the lane-trim ramp** (`Tests/GainProcessorTests.cpp`) that
  would NOT fail if the unity skip were deleted, because the settled gain is
  exactly 1.0 and multiplying by it changes nothing.
- **`isUnity` is never tested at its boundary** -- 0.0 and ±0.5 against a 0.001
  tolerance. Widening the tolerance to 0.4 passes.
- **The ramp-continuity test starts at `i = 1`**, so it never sees the
  block-boundary discontinuity it exists to catch.
- **A two-element tie-break test** would pass with the tie-break removed, because
  both standard libraries use a stable insertion sort at that size. The
  three-element version beside it is what actually catches it.
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

- **`PluginScan::run` writes once, after every format.** A plugin that hard-crashes
  the scan therefore loses every plugin found in that run. The dead man's pedal
  means the next run skips the offender, so N crashing plugins costs N+1 full
  rescans. Writing incrementally would cost a settings flush per format.
- **`--scan` combined with `-self-test`** writes the plugin list into the
  throwaway self-test folder. Harmless, and one guard would make it an error.
- **`SampleRatePolicy` takes `availableRates.getLast()`** as the highest rate the
  device admits to. Every backend here returns them ascending; none contracts it.
  `std::max_element` costs nothing.
- **The signal view has no scrollbar.** 5.3.0 stopped it silently truncating and
  made it count the rows it cannot show. A `juce::Viewport` would show them.

---

## Process

- **Do not quote an assertion count in a comment.** There were four, already
  disagreeing with each other before today: `CMakeLists.txt` and `BACKLOG.md`
  said 1120, `BACKLOG.md` also said 1131, `tools/build-linux-docker.sh` said
  1131. None had been right for weeks. They are gone, and the prose around them
  now says what it means without a number. The figure changes every time anyone
  adds a test; run the binary.

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
