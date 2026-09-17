# Backlog

Open items after 5.4.0. They are written to be picked up cold: what is wrong,
where, why it matters, and what the fix is.

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

Nothing here blocks a release. What remains is two testability gaps, one
decision that needs writing down rather than fixing, and a handful of items
that are a judgement call about assets and scope rather than defects.

The pattern from the last three passes held again: of the work done for 5.4.0,
**five of the defects fixed had no backlog entry at all** -- they were found
while fixing the ones that did. Auditing a change, not only the thing it
changed, is what keeps finding them.

---

## Bugs

### A throw from the chain-list XML write: decide, do not fix

`Source/IconMenu.cpp`, the three `activePluginList` mutation sites and
`changeListenerCallback`

This has been carried as a defect for three releases and it is not one. The
entry offered two fixes and demanded one of them; the code has since argued
back, in `changeListenerCallback`, and the argument is better than the entry.

What is true: `removeType` / `addType` / `sendChangeMessage` are each wrapped in
a guard that rolls the settings back. The listener that writes the XML runs
later on the message thread, outside all three, and a throw from *that* write is
documented as unrecoverable and left to terminate -- deliberately, because
swallowing it would leave a settings file describing a chain that is not
running, and the next launch would restore the wrong one.

The entry's objection was "a guard that covers half a transaction is worse than
one that covers none of it". That is the part that does not survive: these are
two different failures, not two halves of one. The mutation throwing is
recoverable and is recovered from; the persist throwing is not and says so.
5.4.0 brought the third site into the same shape as the other two, so the
asymmetry the entry complained about is gone in the other direction.

**Action: move this to [DECISIONS.md](DECISIONS.md) as a decision with its
reversing trigger** -- which would be a persist that can fail recoverably, i.e.
if the write ever moves somewhere a retry makes sense. Nothing to do in the
code.

---

## Suspected, not traced

### A plugin could drive a self-sustaining rewire loop

`Source/IconMenu.cpp`, `reconnectGraph` against `audioProcessorChanged`

A plugin that re-announces its latency in response to something `reconnectGraph`
does would drive `reconnectGraph` to `audioProcessorChanged` to
`triggerAsyncUpdate` and round again, one turn per message-loop iteration. The
`AsyncUpdater` coalesces, so it would rebuild forever rather than blow the
stack, and `handleAsyncUpdate` logs on every turn, so a live loop would at least
be visible in `LightHost.log` as a repeating line.

5.3.0 guarded `setBypassed` on the value actually changing, which closes the
obvious route -- a bypass write can no longer trigger a latency announcement
that triggers another bypass write. What is not closed is a plugin that
re-declares on `prepareToPlay`.

Not reproduced. It needs a plugin that behaves this way, and none of the sixteen
installed here does.

---

## Testability

Two gaps, both in `PreferencesContentComponent`, and both for the same reason.

- **The committed-chain callback** (`committedChainNamesFn`) has no assertions.
  Its null-callback fallback and the rule that an empty *result* is not a
  fallback case are both unasserted.
- **The device-choice recording** (`deviceChoiceIsUserMade`) has none either.
  The whole point of the 5.3.0 device fix -- that only a combo `onChange` sets
  the flag, and that it clears only after the record is written -- rests on
  reading the code.

**Why they are still open when the other two closed.** The parallel-vector
invariant and the checkbox press/release contract were in
`AudioChainListComponent`, which 5.4.0 lifted into its own header precisely
because it was constructible. These two are in `PreferencesContentComponent`,
which is not: it is defined inside a `.cpp` that the test target does not
compile, its constructor dereferences `juce::JUCEApplication::getInstance()`
(null in a console test), it calls `getAppProperties()` which is defined in
`HostStartup.cpp`, and it takes an `AudioDeviceManager&` while the test target
deliberately does not link `juce_audio_devices`.

Four blockers, and the cheapest is not "extract it too" -- it is a much larger
class with real dependencies. The honest options are to extract the two
*policies* (what counts as a deliberate device choice; how a row is labelled)
as free functions the way `reconcileStagedChain` was, or to accept that these
stay read-verified. Recorded so nobody re-proposes extracting the whole class.

---

## Hardening

- **`tools/audio-endpoints.ps1` throws instead of reporting.** Found by running
  it on an RDP session, where a role legitimately has no default endpoint:

      Exception calling "Report": "Element not found. (0x80070490)"

  The guard beside it is written correctly and **cannot fire**.
  `GetDefaultAudioEndpoint` is declared returning `int` but **without
  `[PreserveSig]`**, so the CLR's COM marshaller turns a failing HRESULT into an
  exception before `if (... == 0 && d != null)` ever runs. Same shape as the
  five gates fixed in 5.3.0: a check that looks right and is unreachable. Fix is
  the attribute, on that method and on `EnumAudioEndpoints` beside it.
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

- **The README's front-page screenshot shows v4.0.3.**
  `docs/images/preferences.png` predates the lane trims, the signal view and the
  status row, so the first thing a visitor sees is three releases out of date.

  5.4.0 removed the hard part: `-preferences` brings the window up from a
  command line, so no tray hunting and no GUI automation is needed.

      "Light Host" -preferences

  What is still needed is a **console session with real audio hardware**.
  Attempted over RDP on 2026-09-17 and abandoned twice. Windows exposes only
  "Remote Audio" and no input to an RDP session, so the device rows would
  misrepresent the application; and `SetForegroundWindow` fails silently when
  the calling process lacks foreground rights, so the capture photographed
  whatever window was painted at those coordinates instead. That second failure
  is the one to remember: it nearly committed an unrelated application's screen
  content into a public repository, and the only thing that caught it was
  looking at the image before using it. **Check any automated capture by eye.**
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

## How 5.4.0's eleven were cleared

On 2026-09-17, the same day 5.3.0 shipped. Six dispositions again, and the
count that matters is the last row: **five defects were fixed that no entry
described**, found while fixing the ones that did.

| How | Count | Where it went |
|---|---|---|
| Fixed | 6 | [CHANGELOG.md](CHANGELOG.md), `## [5.4.0]` |
| Dissolved by a design change | 1 | the drag-reorder guard -- three parallel vectors became one, so the asymmetry is unrepresentable |
| Was never a defect | 1 | the two-element tie-break test; sort stability makes it fire, so the premise was backwards |
| Partly closed, remainder restated | 1 | the missing Preferences tests: two of four gaps closed, two moved to Testability above with the reason |
| Restated as a decision | 1 | the uncaught XML write, above |
| Still open | 1 | `--scan` with `-self-test` |
| **Found and fixed with no entry** | **5** | below |

### The five with no entry

Three of them are the same fault in three places: `handleDeletePlugin` and both
loops in `applyPluginChain` deleted a plugin's **preset file before** a guarded
mutation that can roll the settings back. A throw left the preset gone with the
settings restored, pointing at a plugin whose saved state no longer existed.

The fourth: `reconcileStagedChain` shipped keeping staged additions and
discarding staged deletions. Asymmetric for no reason a user could discover, and
the deleted row came back at the *end* of the list rather than where it was.

The fifth: `pressAt` computed its own row index instead of using `rowAt`.
Integer division truncates toward zero, so a press ten pixels above the list
gave row 0 and the bounds check accepted it -- arming a drag on the first
plugin. `rowAt`, used by the hover path, guarded `y < 0` correctly, so the two
hit tests disagreed about the same point.

### One test that had to be written twice

The negative-y test first asserted that the checkbox did not toggle. It passed
against the deliberately broken build, because the bad point lies *outside* the
checkbox rectangle and falls through to the drag branch. The observable is the
reorder, not the toggle. Worth recording: a mutation that does not fire is
usually the test being wrong, not the bug being absent.

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
