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

Nothing here blocks a release, and nothing here is broken. What remains is five
testability gaps -- each one a check that exists and cannot be fired, listed so
nobody trusts it without knowing that -- two small layout defects, and four
defensive notes about code that is correct today for a stated reason.

Everything that was actually wrong has been fixed. The three bugs and four
suspicions carried into 2026-09-18 are all closed above, along with every
hardening item that turned out to be a one-liner.

The pattern from the last three passes held again: of the work done for 5.4.0,
**five of the defects fixed had no backlog entry at all** -- they were found
while fixing the ones that did. Auditing a change, not only the thing it
changed, is what keeps finding them.

---

## Bugs

**All three are fixed.** Traced and closed on 2026-09-18, along with the four
suspicions below. Two of the three turned out to be cheaper than this file
predicted, and one of the suspicions was a real defect the entry had
mis-diagnosed.

### Fixed: a truncated `.lhs` is no longer handed over as a partial preset

`Source/PluginStateVault.hpp`, `read`

The entry said detection "needs a length or a checksum in the header, which is a
format change with a migration for every `.lhs` already on disk." **That was
wrong, and it is worth understanding why, because the same reasoning error is
easy to repeat.** It looked at the *gzip* trailer and correctly found no way to
ask `GZIPDecompressorInputStream` whether the stream ended cleanly. But the
payload is not gzip: `GZIPCompressorOutputStream` defaults to `windowBits = 0`
and the decompressor to `zlibFormat`, so it is a **zlib** stream, and RFC 1950
ends one with a four-byte big-endian Adler-32 of the uncompressed data.

The integrity field was already in the file. No format change, no migration.

`read` now computes the Adler-32 of what decompressed and compares it with the
trailer. On a truncated file the real trailer is gone, so the comparison is
against whatever deflate bytes landed last — which disagree unless they collide
with the true checksum, about one chance in four billion. A probabilistic check,
which is stated in the code, and a large improvement on handing half a preset to
`setStateInformation`.

It also returns a `ReadResult`, because `corrupt` must not be confused with
`absent`: a damaged file is still the only copy of that preset, so the node is
recorded as un-restored and the file is left alone. Collapsing those two is
exactly how 5.2.0 lost a preset through the legacy base64 path.

The test that pinned the old behaviour has been inverted, as its own comment
asked. `adler32` is tested directly against the RFC's published values, and its
NMAX blocking against a naive per-byte implementation.

### Fixed: `committedBaseline` no longer records a chain the apply did not commit

`Source/PreferencesWindow.cpp`

Write site 3 is now conditional. `setChain` counts its own writes, and the apply
lambda skips its write when that counter moved while it was inside the host —
because the only way it moves there is the abandon path refreshing the panel,
and that refresh has already set the baseline from the chain actually committed.
On the success path a refresh sets what `stagedRows` would have, so deferring
costs nothing.

Chosen over making `onApplyFn` return a result, which would change the apply
path's contract. The member comment that claimed three unconditional write sites
now says site 3 is conditional and why.

### Fixed: the vault erase is transactional across the whole edit

`Source/IconMenu.cpp`, `applyPluginChain`

Both erase loops — `departing` and `dropped` — now collect identities into a
list instead of deleting as they go, and the deletes happen after
`store.commit()` and `flushSettings()` have succeeded. Every `abandon` path
returns before that list is walked, so a rolled-back edit deletes nothing.

This did not need the transaction redesign the entry predicted. Staging inside
`ChainStore` would have meant a `commit()` signature change across six call
sites; a local vector in the one function that has the problem is the same
guarantee with none of that. Note `deletePluginStates` deliberately still
deletes directly — its whole purpose is deleting, it counts per-file failures to
report them, and it has no mutation that can throw.

The `abandon` log line said "settings untouched", which was not quite true: the
change listener rewrites `pluginListActive` outside the guard by design. It now
says what actually happened.

### A throw from the chain-list XML write: settled, see DECISIONS.md

`Source/IconMenu.cpp`, the three `activePluginList` mutation sites and
`changeListenerCallback`

Kept as a signpost only. This was carried as a defect for three releases and is
not one: the mutation throwing is recoverable and is recovered from; the persist
throwing is not, and says so. It is in [DECISIONS.md](DECISIONS.md) with its
reversing trigger — a persist that can fail recoverably.

---

## Suspected, traced 2026-09-18

All four resolved. Two were not real, one was real and got fixed, one was real,
unreachable, and got a guard anyway.

### NOT REAL: a plugin driving a self-sustaining rewire loop

The named route — a plugin that re-declares latency in `prepareToPlay` — is
unreachable from `reconnectGraph`, because **a rebuild never re-prepares a node
that is already prepared.** `NodeStates::applySettings` keeps a `preparedNodes`
set and `continue`s past anything in it, and that set is only cleared when the
graph's sample rate or block size actually changes — which `rebuild()` does not
touch. So each turn of the supposed loop prepares nothing and announces nothing.

Two further dampers sit behind that: the render-sequence signature means a
rewire that changes nothing publishes nothing, and `setLatencySamples` is itself
change-guarded, so a plugin re-announcing the *same* latency emits no callback.

Observed live on the real chain: one plugin does re-declare its latency after
load, producing exactly one extra rewire, which then settles. That is the
desired behaviour — it is what picks the new latency up.

No budget was added. The sample-rate recursion it would imitate is genuinely
unbounded; this one is provably not, and a second piece of state to keep correct
for a loop that cannot start is a cost with no benefit.

**One residual hole was real and is now closed.** `Node::isBypassed()` reads the
plugin's own bypass *parameter*, not our stored intent, so a plugin that clamps
or ignores the write leaves the guard's condition true for ever and is written on
every single rewire. `reconnectGraph` now reads back once and, if the write did
not take, logs it and stops asking that node for the rest of the session. That
bounds the one unbounded write into third-party code, and surfaces a plugin bug
the user would otherwise never see.

### REAL, FIXED: every plugin add published a render sequence

The `addNode` in `onPluginInstanceReady` — not `loadActivePlugins`, as the entry
said — was the only graph mutation in the file taking the default `UpdateKind`,
which is `sync`. An eight-plugin chain therefore published ten render sequences
during load where two would do, each an O(nodes²) ordering pass plus three
block-sized buffer allocations. Now `UpdateKind::none`, like every other
mutation there.

It also moves `prepareToPlay` to the final rebuild, i.e. **after**
`restorePluginState` rather than before it, which is the better order — a plugin
sizes its buffers knowing its real settings — and the order `OfflineRender`
already used. Verified on the real chain: all plugins load, the graph wires, and
the render regression is byte-identical.

Still not measured as a startup-time improvement, and it is not claimed as one.
It is provably wasted work removed.

The four defaulting sites in `OfflineRender.hpp` are genuinely free, because
they all run before `graph.prepareToPlay` when there are no settings to apply,
so no sequence is built.

### REAL, FIXED — and the entry had the cause wrong

`~IconMenu` did have a hole here, but not the one described. The claim was that
`cancelPendingUpdate()` runs before its most likely trigger and that
`~AsyncUpdater` cancels again anyway, making it cosmetic. Both halves are wrong:
`IconMenu` derives from `AsyncUpdater`, so that destructor runs *after* this
whole body, far too late to help.

What was actually missing is `stopListeningToAll()`. `loadActivePlugins` calls it
immediately after its cancel, and `PluginWindow.cpp` names that pairing as the
caller's responsibility — because `closeAllCurrentlyOpenWindows()` deletes every
editor and then **pumps the message queue**. Still registered as a listener, a
latency report from a closing editor was delivered inside that pump, running
`reconnectGraph()` mid-teardown — which adds probe nodes to a graph about to be
destroyed and pushes a bypass parameter into a plugin whose editor has just
gone — and `refreshPreferencesIfOpen()`, which drives a window this destructor
deliberately never resets.

One line added. Moving the cancel, which is what the entry proposed, would have
fixed nothing: the re-arm and its delivery are both inside
`closeAllCurrentlyOpenWindows()`.

### REAL BUT UNREACHABLE, GUARDED: the node id counter had no ceiling

Verified rather than assumed: there is **no bulk-allocation path.** Both callers
allocate only for a plugin with no stored id, and the abandon path rolls a
staged allocation back. Reaching `1'000'000` needs roughly that many discrete
adds.

Guarded anyway, because the failure is silent in a release build and does not
look like an id problem. `addNode` refuses a duplicate id with only a debug
assertion, and `getNodeForId (1'000'000)` then returns the graph's audio **input**
node for that plugin — so the chain wires input to input, and if that plugin is
bypassed it bypasses the graph input and silences the host entirely.

`allocateNodeId` now stops at `kMaxPluginNodeId` and returns 0, which is the
value `readNodeId` already uses for "no id yet". Both callers check it and report
rather than passing it on. Two tests pin the ceiling, and one asserts it against
`NodeIds.hpp` rather than against a second copy of the number, so raising one
without the other goes red.

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

### Three checks added in 5.4.0 that the suite cannot exercise

Listed rather than left implied, because a check nobody can fire is a check
nobody should trust without saying so.

- **`Vault::write`'s `out.getStatus()` test.** Correct by construction, and
  reachable only on a real I/O failure: a full disk, a device pulled mid-write.
  The suite cannot produce one, because `write` constructs its own
  `FileOutputStream` and there is no seam to inject a failing stream through.
  Making it testable means taking the stream as a parameter or behind a factory,
  which is a bigger change than the fix was.
- **`deletePluginStates`'s failure count and its honest message.** Needs an
  `IconMenu`, real settings, and a `.lhs` held open by another process. The
  message is now derived from a count rather than printed unconditionally, so
  the *shape* is right, but no test proves the count reaches the user.
- **The `getCommittedChainNames` cap at `nodeids::maxProbes`.** Needs a 33-plugin
  chain to distinguish capped from uncapped, and the signal view pairs names to
  meters one to one, so it also needs the panel.

---

## Hardening

- **FIXED 2026-09-18: `tools/audio-endpoints.ps1` reports instead of throwing.**
  Every method on all four COM interfaces now carries `[PreserveSig]`, and the
  calls that were only survivable because the marshaller raised now check their
  return. Verified by firing it: asking for role 99 returns `0x80070057` with a
  null device, where it previously raised. Found by running
  it on an RDP session, where a role legitimately has no default endpoint:

      Exception calling "Report": "Element not found. (0x80070490)"

  The guard beside it is written correctly and **cannot fire**.
  `GetDefaultAudioEndpoint` is declared returning `int` but **without
  `[PreserveSig]`**, so the CLR's COM marshaller turns a failing HRESULT into an
  exception before `if (... == 0 && d != null)` ever runs. Same shape as the
  five gates fixed in 5.3.0: a check that looks right and is unreachable. Fix is
  the attribute, on that method and on `EnumAudioEndpoints` beside it.
- **FIXED 2026-09-18: `--scan` with `-self-test` is refused**, with the reason
  and exit 2, and both flags are disarmed before quitting so the self-test's
  shutdown checks do not print a contradictory failure underneath. It used to
  write the plugin list into the
  throwaway self-test folder. Harmless, and one guard would make it an error.
- **CLOSED 2026-09-18: the signal view scrolls.** The rows moved into a
  `juce::Viewport`, following the `chainViewport` pattern rather than
  `PreferencesPanelViewport` -- the latter exists to own a heap-allocated panel
  handed to `setContentOwned`, and these rows are a by-value member.

  The scrollbar thickness is subtracted only when the bar is actually shown,
  which `updateChainListHeight` does unconditionally. That costs the chain list
  nothing because its width follows the window, but this column is a fixed 300px
  and pins its numbers to the right edge, so surrendering 8px for an absent bar
  would move every number in the common case.

  The 5.3.0 "N more not shown -- make the window taller" message is gone,
  because it is no longer true: every row is reachable. The accounting was
  repointed rather than deleted, at the limit a scrollbar cannot help with --
  `nodeids::maxProbes`, 32, where `getCommittedChainNames` caps the list so
  names and meters stay the same length. The header now says "at the 32-probe
  limit" rather than naming a count of hidden rows, because a list that comes
  back at exactly 32 is either a 32-plugin chain with nothing missing or a
  longer one with its tail dropped, and the panel cannot tell which.

  Per-row meter watches deliberately still follow the column's visibility, not
  the scroll position. The delta column reads the row above, so gating on
  visibility would make the topmost visible row lose its reference while
  scrolling -- and the fall ballistics advance per tick, so a row scrolled back
  would carry a stale peak. `paint` does now clip to `getClipBounds()`, so a
  34-row chain costs the visible handful of rows per frame instead of all 34.

  This also answers the 150%-display-scaling question. The main panel already
  lives in a scrolling viewport, so it degrades by scrolling rather than
  clipping; the signal view was the only part that did not, and now does. At
  150% the default 520x650 window asks for 780x975, which fits a 1440 panel with
  room and fits 1080p as well.

- **FIXED 2026-09-18: `tools/render-regression.sh` no longer pipes into
  `head`.** `awk` and `sed` both stop on their own, so neither pipe needed to
  exist. It used to pipe `grep` into `head -n 1` under
  `pipefail`.** If `grep` were killed by SIGPIPE after `head` exited, `pipefail`
  would yield 141 and `set -e` would abort the script -- a tooling failure
  reported as a regression. Not reachable with the current three-line baseline
  file, where `grep` finishes before `head` closes. Worth knowing if that file
  ever grows.
- **FIXED 2026-09-18: minimise and restore no longer stops the meter timers.**
  Found while
  adding the scrollbar, and pre-existing. `SignalMeter::timerCallback` and
  `SignalViewRows::timerCallback` stop correctly when the window stops being
  visible, which is what keeps a minimised window free -- but nothing started
  them again. JUCE delivers `minimisationStateChanged` to the top-level
  component only, and restoring produces no `visibilityChanged`, no
  `parentHierarchyChanged` and no `resized()` below it, because the peer skips
  its bounds update while minimised so the bounds are unchanged on restore.

  **FIXED and now tested.** A `VisibilityDrivenTimer` interface, implemented by
  both timer owners, and `PreferencesWindow::minimisationStateChanged` -- the one
  component that receives the event -- walks the tree and tells them to
  re-check. `SignalViewRows` rebuilds its `Meter::Watch` objects as well as
  restarting its timer, because `setWatching(true)` goes through
  `beginWatching()`.

  It was recorded here as "reasoned rather than observed" for a few hours, and
  that is now closed. The walk lives in `Source/VisibilityTimers.hpp` so it is
  reachable from tests, and `Tests/VisibilityTimerTests.cpp` asserts both halves:
  seven headless cases for the walk, and two GUI cases that minimise and restore
  a real `DocumentWindow`. The second GUI case is the interesting one -- it
  asserts that a descendant receives NO `visibilityChanged`, NO
  `parentHierarchyChanged` and NO `resized()` on restore, which is the claim the
  whole fix rests on. If JUCE ever starts telling descendants directly, that
  test goes red and the forwarding becomes redundant, which is worth being told
  rather than left to be rediscovered.

  Both were confirmed to fail before being believed: making the walk
  non-recursive turns the nested case red with "a timer three levels down was
  not reached".

- **`setStatusMessage` can push the Apply button off the bottom.** It grows
  `fixedLayoutHeight()` by a row and calls `resized()` on the panel, which
  absorbs it from the chain viewport -- unless that viewport is already at
  `kMinChainH`, in which case the lower sections overflow. Nothing re-runs
  `PreferencesPanelViewport::resized`, so the panel is never re-heighted and the
  viewport never learns it has more to scroll. Same shape for the Windows
  virtual-input hint. Not fixed: it needs the panel re-heighted from the status
  path, which is the layout plumbing rather than a one-liner.

- **`updateChainListHeight` subtracts the scrollbar width unconditionally.**
  Cosmetic for an elastic-width list, and named only because it is the one point
  where this file's two viewport patterns now disagree -- the signal view
  subtracts it conditionally, for a reason that does not apply here.

- **`chooseChainTestOutput` destroys its `FileChooser` from inside that
  chooser's own callback**, by reassigning `chainTestChooser`. It survives
  because `FileChooser::finished` copies the callback out and touches nothing
  after invoking it, and because the callback reads `fc.getResult()` before the
  reassignment. Correct today, and resting on the internals of a vendored class
  that a JUCE bump could change.
- **`AudioChainListComponent::setRows` does not reset `hotRow`/`hotControl`.**
  It resets the drag and pressed state but not the hover. Cosmetic only: `isHot`
  is consulted for `i < rows.size()`, so a stale value never matches, and
  `repaintRow` on an off-list index is a no-op.
- **`SignalViewPanel::setTaps` clears its watches without stopping the timer**
  when the panel is not showing. Self-heals on the next tick via the `isShowing`
  check, so it costs one wasted callback.
- **`updateChainListHeight` early-returns when the viewport has no width**,
  leaving the list at its previous height. Only reachable before the first
  layout.

---

## Process

- **DONE 2026-09-18: the README screenshots are current.** Captured by hand on
  a console session, which is what the two failed RDP attempts established was
  necessary. Nine new images; the three v4.0.3-era ones are no longer
  referenced.

  The lesson from the failed attempts is kept because it is the useful part:
  `SetForegroundWindow` fails silently when the calling process lacks foreground
  rights, so an automated capture photographed whatever window was painted at
  those coordinates. It nearly put an unrelated application's screen content
  into a public repository, and the only thing that caught it was looking at the
  image. **Check any capture by eye before referencing it.** Every one of the
  nine was viewed and audited before being embedded.

- **CLOSED 2026-09-18: the handle in git history is accepted.** The old
  front-page shot carries "J-Dizzle Mic Chain" as a device name, in history from
  `c7aba83`. The repo owner's decision is that a self-chosen handle is not a
  concern, so nothing is being rewritten. Recorded because the alternative --
  rewriting a public repository and breaking every clone -- should never be done
  on a whim later by someone who finds this and assumes it was an oversight.

- **CLOSED 2026-09-18: the orphaned images are resolved.**
  `plugin-scanning.png` is now used: it shows the two default VST2 scan
  locations, which is direct evidence for the `--scan-path` paragraph that had
  only prose behind it. `preferences.png`, `available-plugins.png` and
  `options-menu.png` are deleted from the tree -- all three are 4.0.3-era, all
  three are superseded, and all three remain in history if wanted.

- **FIXED 2026-09-18: `RELEASING.md` uses `v5.4.0` as its worked example**, with
  a note explaining why the old one was a bad choice. It used `v5.2.0`, and that is
  the one version in the v5 line that was deliberately never tagged despite
  having a CHANGELOG section. So the example version doubles as the
  counter-example, and a reader checking the doc against `git tag` finds the
  example missing. Harmless; pick a version that exists next time the file is
  touched.
- **Do not quote an assertion count in a comment.** There were four, already
  disagreeing with each other before today: `CMakeLists.txt` and `BACKLOG.md`
  said 1120, `BACKLOG.md` also said 1131, `tools/build-linux-docker.sh` said
  1131. None had been right for weeks. They are gone, and the prose around them
  now says what it means without a number. The figure changes every time anyone
  adds a test; run the binary. This file quoted one two lines above the item
  saying not to, until 2026-09-17.
- **DONE 2026-09-18: `Resources/icon.png` is 335,036 bytes**, down from
  1,055,725 (-68%), and nothing displayed changed. The open question was whether
  this repo should carry a 1024 px original at all; the answer taken is no,
  because nothing consumes it above 512.

  The entry that stood here reasoned from "nothing above 256 px ever reaches the
  binary", which is true of Windows only -- `writeWinIcon` emits
  {16, 32, 48, 256} but `writeMacIcon` emits {16, 32, 64, 128, 256, 512, 1024}.
  A 256 px source would have quietly dropped two sizes from the macOS `.icns`.
  512 keeps every size but the Retina-largest, and the shipped Windows 256 entry
  is 50.4 dB PSNR against the old one.

  The 1024 master is in history and recoverable with
  `git show 1f4acb2:Resources/icon.png` if it is ever wanted back.
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
- **Display scaling at 150% is answered; the light taskbar still needs eyes.**

  The scaling half is no longer an open question. The whole Preferences panel
  sits in a scrolling viewport, so it degrades by scrolling rather than
  clipping, and since 2026-09-18 the signal view -- the only part that did not --
  scrolls too. At 150% the default 520x650 window asks for 780x975, which fits
  a 1440 panel with room and fits 1080p; the 440x400 minimum becomes 660x600.
  So there is no size at which the layout has nowhere to go.

  What that does NOT prove is that every control looks right at 150%, only that
  nothing is unreachable. The development machine runs at 100% (96 DPI), and
  nobody has looked at it scaled.

  The tray icon against a light taskbar cannot be reasoned about at all -- it is
  one bitmap for both themes. Both remain pre-release checks in
  [RELEASING.md](RELEASING.md).
- **macOS is built only by CI.** Linux and Clang can now be run before a push --
  `tools/build-linux-docker.sh` and the `clang-release` preset -- so macOS is the
  only platform whose first evidence still arrives after the commit. Its Clang is
  close enough to the local one that they rarely disagree, and rarely is not
  never.

---

## What the 5.4.0 audit round found

Three read-only audits over disjoint areas -- audio and graph, UI and persisted
state, tests and tooling and docs -- run against `3a2660a` after the release
commit was green and before the tag. None of what follows had an entry here.
Everything marked fixed is in `## [5.4.0]` of [CHANGELOG.md](CHANGELOG.md);
everything marked open is written up above.

Eleven guards were mutation-tested: break the thing each protects, confirm the
right test fails, restore. All eleven fired. The harness asserts the built
artefact's timestamp advanced before believing any verdict, because a mutation
that silently was not rebuilt reads exactly like a guard that works.

| Found | Disposition |
|---|---|
| A muted lane trim was bypassed on every startup, so audio passed at unity while plugins loaded | Fixed -- passthrough now runs through lane 0's trim |
| An empty `getStateInformation` made `Vault::write` DELETE the preset and return true | Fixed -- empty is refused; erasing stays a separate verb |
| A truncated write returned true, because the gzip tail is flushed from a destructor nobody checks | Fixed -- `flush()` then `getStatus()` before the rename |
| Three `[[nodiscard]]` erases discarded, and "Deleted saved plugin states" could not fail | Fixed -- one helper, a failure count, an honest message |
| The add menu could stage one identity twice, and two comments said it could not | Fixed -- `addRow` de-duplicates; the comments now match |
| `addRow` was the only mutator that skipped `onChange` | Fixed |
| The peak meter decayed per block, so its rate was right only at 480 samples and 48 kHz | Fixed -- `Meter::setTimebase`, measured at four rate/size pairs |
| Three collapsed assertions swallowed NaN, because `jmax(a, NaN)` returns `a` | Fixed -- NaN-propagating accumulator |
| A test written with `juce::UnitTest("name")` never ran and nothing noticed | Fixed -- the guard walks `getAllTests()`, not `getAllCategories()` |
| Losing `xvfb-run` let Linux CI report success having run one test of four | Fixed -- `LIGHTHOST_REQUIRE_SMOKE_TESTS`, on in both workflows |
| `ci-status.sh` reported a `gh` failure as a CI verdict, on the one call it left unguarded | Fixed |
| `update-juce.sh` hardcoded `JUCE 9`, so a major bump rewrote nothing and exited 0 | Fixed -- any major, and rewriting nothing is now fatal |
| `build-linux-docker.sh` computed a `--user` argument and passed it to neither `docker run` | Fixed |
| `-preferences` accepted one spelling of two, and was the only flag with no test | Fixed -- `Source/StartupFlags.hpp` |
| `getCommittedChainNames` was unbounded while `getProbeMeter` caps at 32 | Fixed -- capped, so a 33rd row cannot draw permanent silence |
| `OfflineRender`'s `graphChannels` could only ever be 2, and read as though it handled mono | Fixed -- render hash unchanged |
| The realtime "takes no locks" claim rested on unasserted lock-freedom | Fixed -- three `static_assert`s |
| A truncated `.lhs` still yields a partial preset on read | **Open** -- needs a format change; above |
| `committedBaseline` can record a chain the apply did not commit | **Open** -- `bad_alloc` only; above |
| The vault-erase reorder is per iteration, not per transaction | **Open** -- `bad_alloc` only; above |
| Plugin adds publish a render sequence each; the id counter has no ceiling; the destructor's cancel is placed oddly | **Open, unmeasured** -- above |

### Six documents said things that were not true

Worth separating, because every one of them was written in the same pass that
built the thing it described, and five of the six shipped in the v5.4.0 release
body or the archives.

- `CHANGELOG.md` said `tools/README.md` indexed fourteen scripts "none of which
  was referenced by any document". Twelve were; the true claim is the narrower
  one the index itself makes. **Corrected.**
- `CHANGELOG.md` counted four tests as tightened. Three were; the fourth was
  investigated and deliberately left alone, which this file already recorded
  correctly. The two documents contradicted each other. **Corrected.**
- This file counted the XML-write item as cleared in the table below *and*
  carried it above as an open bug with an action attached. **Corrected.**
- `RELEASING.md` said the release process had run three times. Four.
  **Corrected.**
- `tools/README.md` said four `.ps1` files compile C# at run time. Three do.
  **Corrected.**
- `README.md`'s test-coverage list omitted the chain list and the reconcile
  entirely -- the largest test addition of the release. **Corrected.**

Two code comments also described mutations that do not produce the effect
claimed, and one justified a defensive branch by a historic bug that could not
have happened. All three were checked by running the mutation rather than by
re-reading the code, and rewritten to what was measured.

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
