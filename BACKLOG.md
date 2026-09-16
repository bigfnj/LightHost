# Backlog

## Open items

**None.**

Emptied 2026-09-16 for 5.2.0, from 24 items. "Empty" was reached four different
ways, and the difference matters more than the count:

| How | Count | Where it went |
|---|---|---|
| Fixed | 10 | [CHANGELOG.md](CHANGELOG.md), `## [5.2.0]` |
| Made testable instead of checked by hand | 2 | `Tests/ConfirmPolicyTests.cpp`, `Tests/PluginWindowTests.cpp` |
| Already shipped, or the premise did not hold | 4 | see below |
| Declined, with reasoning and a reversing trigger | 8 | [DECISIONS.md](DECISIONS.md) |

Adding to this file is expected and good. It is empty because the items in it
were dealt with, not because nothing is ever worth writing down.

### Two of them were misdiagnosed, which is worth remembering

**"VST2 scanning does not descend into subdirectories" was not a defect.**
`PluginDirectoryScanner` is constructed with `recursive = true` in both scan
paths — JUCE hard-codes it for its own scan, and our custom-folder scan passes
it — and the nine ReaPlugs DLLs were already present in the scanned list, found
from the parent folder one level down, while the stored search path named only
that parent. The entry appears to have been written from looking at the folder
picker rather than from a scan that missed files. Two real defects were found
next to it and fixed instead: failed plugin loads were discarded, and a custom
folder was never stored so it was never re-scanned.

**"A nested dispatch loop runs in the middle of a graph teardown" overstated
where it runs.** The message pump in `PluginWindow::closeAllCurrentlyOpenWindows`
runs *before* `graph.clear()`, so the graph is whole throughout — it sits between
editor teardown and graph teardown, not inside one. The real gap was next to it
and is fixed: the chain reload never cancelled its async updater, which a hosted
plugin triggers when it reports a latency change on the way out. The pump itself
is kept and now documented. Removing it would need a soak across real VST2 and
browser-hosting editors, because whether any plugin needs a message-loop turn
between its editor and its processor being destroyed is not answerable by reading
code.

### Three were stale rather than outstanding

Per-plugin and device metering shipped in 5.1.0. Rewriting every plugin's state
on every edit stopped in 5.0.0, when state moved to a file per plugin. The
release procedure works — three releases have gone through it — and
`RELEASING.md` has been rewritten to describe what is actually done rather than
a release-candidate process that is not used.

---

## What has been verified, and how

Kept because these were open questions for a long time and the answers should not
have to be rediscovered. This is a record, not a work list.

### Verified 2026-09-16 (5.2.0)

- **The device-substitution report works on a real fallback.** The stored request
  was pointed at a device that does not exist and the application launched
  against it: both the input and the output substitution were named in the log
  and raised through the status sink. Both sides move, because JUCE falls back to
  the default device for each — which is what made the original incident send a
  microphone chain to room speakers.
- **It does not cry wolf.** The same build launched against the real settings,
  with both requested devices present, reported nothing.
- **The live chain is not byte-reproducible, and the regression gate does not
  pretend otherwise.** Four paced renders of one input: ReaEQ gave one hash 4/4
  times, Salvor gave two. This happens while the renderer reports a realtime
  factor of 1.000 with no blocks behind, so "kept pace" does not imply
  reproducible — the inference runs on a worker thread and lands on slightly
  different block boundaries. `tools/render-regression.sh` therefore gates on a
  deterministic reference chain, which still exercises everything in the host.
- **Nothing in 5.2.0 changed a sample.** The render regression was checked after
  every phase and stayed identical throughout.
- **The confirmation dialog's button order is now checked by CI on all three
  platforms**, rather than needing a person at a Linux machine to dismiss a
  dialog and see what happened.

### Verified 2026-09-16 (5.1.0)

- **Plugin state is no longer copied three times per load.** It is moved into the
  async creation lambda; a real state measured 4,126,524 bytes, so a five-plugin
  chain was copying tens of megabytes on the message thread at startup.
- **The self-test log poll re-reads only when the file grows**, rather than
  reading and scanning a 256 KB file up to 200 times per run.
- **Metering does not alter the audio.** A fixed input renders byte-identical
  through `--render` before and after every phase of the 5.1.0 work, and a
  topology test asserts that a chain with no probes wires exactly as it did
  before probes existed.
- **Probe insertion works on a live graph.** The startup self-test opens the
  signal view part way through its run, so adding nodes to a running graph and
  rewiring it is exercised on all three platforms in CI.
- **JUCE 9.0.2 is clean for this project.** Its one breaking change removes an
  API this codebase does not use.

### Verified 2026-09-08

- **Real plugins load and run.** Salvor, smartChain, Alt Denoiser and three Elgato
  plugins have all been instantiated through this code, live and through the
  offline render. Bypass, lane moves, quit and relaunch all survive; order, lanes
  and state persist.
- **The settings migration ran on a real settings file.** 7,489,165 bytes of
  base64 plugin state migrated to the vault, leaving roughly twelve kilobytes of
  readable XML. Not synthetic data.
- **Declared latency is honest.** A chain declaring 4512 samples (94 ms) measured
  80–84 ms by envelope cross-correlation, agreeing within the method's
  resolution.
- **The graph's inter-lane compensation is correct** and the host adds none of its
  own, which is what fixed the double-padding bug from 4.0.3.

---

## Standing checks

Things that are not work items but have to be re-run when something upstream
changes. Automated where possible, so they are not a list someone has to
remember.

- **System-audio loopback capture** is blocked on JUCE exposing it, and
  `tools/update-juce.sh` greps for it on every version bump, so the answer is
  re-checked without anyone deciding to.
- **Display scaling at 150%, and the tray icon against a light taskbar**, cannot
  be automated — they need a person looking at a display that is configured that
  way. Recorded in [RELEASING.md](RELEASING.md) as pre-release checks rather than
  as perpetually open backlog items.
- **Linux and macOS are built by CI on every push**, on all three platforms,
  including the unit, GUI and smoke suites. No developer machine here has ever
  built them, and CI is stronger evidence than a local build would be, so this is
  not an open item.
