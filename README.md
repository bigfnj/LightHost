# Light Host

**Your plugins, on your microphone, all the time — without opening a DAW.**

Light Host is a tray-resident audio plugin host. It takes one audio input, runs it
through a chain of VST/VST3/AU plugins, and sends the result to one audio output.
That is the whole idea. There is no timeline, no mixer, no project file, and no
main window — just a tray icon and the settings panel below.

Point it at your microphone, add a gate, an EQ and a compressor, send the result
to a virtual cable, and every application on the machine hears your processed
voice. Set it up once and forget it is running.

<p align="center">
  <img src="docs/images/lighthost-main.png" alt="The Light Host Preferences window: input device with a level meter and clip badge, an audio chain of three plugins each with a lane and a settings button, lane trims for the four lanes, output device with its own meter, and device settings ending in the latency readout" width="520">
</p>

---

## Contents

- [Why you might want this](#why-you-might-want-this)
- [Quick start](#quick-start)
- [Adding plugins](#adding-plugins)
- [Processing audio from other applications](#processing-audio-from-other-applications-windows)
- [Lanes, trims and delay compensation](#lanes-trims-and-delay-compensation)
- [Seeing your own signal](#seeing-your-own-signal)
- [Testing your chain on a file](#testing-your-chain-on-a-file)
- [Where your settings live](#where-your-settings-live)
- [Running more than one instance](#running-more-than-one-instance)
- [When something goes wrong](#when-something-goes-wrong)
- [Known limitations](#known-limitations)
- [Building from source](#building-from-source)
- [Tests](#tests)
- [Project layout](#project-layout)
- [Licence](#licence)

---

## Why you might want this

- **A permanent processing chain for your microphone.** Every app on the machine
  gets the processed signal, not just the one you have open.
- **No DAW tax.** A DAW to run three plugins on a mic means a project file, a
  transport, and a window you have to keep alive. This is a tray icon.
- **Parallel processing without a mixer.** Up to four lanes, summed at the output,
  with automatic delay compensation so they stay sample-aligned.
- **It stays out of the way.** No update check and no network access of any
  kind. Nothing polls while it sits in the tray: the only timers are a one-shot
  that builds the tray menu and a 25 Hz meter refresh that runs while a meter is
  on screen.

### What it is not

Not a DAW, not a recorder, and not a mixer. There is no MIDI routing, no
side-chain and no recording. Metering is deliberately limited to levels -- see
"Seeing your own signal" below -- so there is no spectrum, no correlation and no
loudness measurement. If you need those, you need a DAW.

---

## Quick start

1. **Download** the archive for your platform from
   [Releases](https://github.com/bigfnj/LightHost/releases), unzip it, and run it.
   Nothing to install; the Windows build is a single self-contained `.exe`.
2. **Left-click the tray icon** to open Preferences.
3. **Pick your input and output** devices at the top and bottom of the panel.
4. **Click `+ Add Plugin`.** The first time, Light Host needs to know what you
   have installed — see [Adding plugins](#adding-plugins).
5. **Click `Apply`.** It will tell you what it did.

Your chain, plugin settings, lane assignments, bypass states, trims and device
choices are all restored next launch.

> **Quit from the tray menu**, not by killing the process. That is what writes
> your plugin settings to disk.

> **Cannot find the tray icon?** Windows hides icons it decides are inactive,
> and Light Host has no other window to fall back on. Start it with
> `-preferences` and it opens the panel itself:
>
> ```
> "Light Host" -preferences
> ```
>
> That is an ordinary run with the window already showing, not a special mode:
> the tray icon, the audio device and your chain all behave exactly as usual.

### The first run shows a warning

Release binaries are not code-signed, so Windows SmartScreen will say it stopped
an unrecognised app the first time you run one. Choose **More info**, then **Run
anyway**. This is expected and it is not a sign that anything is wrong with the
download; verify the archive against the `SHA256SUMS` file on the release if you
want to check what you have. macOS is stricter — see
[Known limitations](#known-limitations).

Signing is a deliberate omission rather than an oversight. A code-signing
certificate is a recurring paid subscription tied to a verified legal identity,
renewed for as long as releases continue, and one SmartScreen click per download
is not worth that to this project.

---

## Adding plugins

`+ Add Plugin` lists what Light Host already knows about, grouped by manufacturer:

<p align="center">
  <img src="docs/images/plugins.png" alt="The Add Plugin menu listing manufacturers — Altinus, Cockos, Elgato, scintillator and sonible — with the Elgato submenu open on its five plugins" width="320">
</p>

To scan for plugins, use **Edit Plugins** from the tray right-click menu:

<p align="center">
  <img src="docs/images/plugins-loaded.png" alt="The Available Plugins window listing scanned VST and VST3 plugins with name, format, category, manufacturer and description columns" width="720">
</p>

The `Options…` button holds the scanning controls, including a **Scan Custom
Folder…** item for plugins installed somewhere non-standard:

<p align="center">
  <img src="docs/images/plugin-options.png" alt="The Options menu open below its button, showing list management and scan actions including Scan Custom Folder, with the two items that need a selected plugin greyed out" width="380">
</p>

Light Host does not prune your scanned list. A plugin that will not load is
reported and skipped, and stays in the list so you can see it is there.

### Supported formats

| Platform | Formats |
|---|---|
| Windows | VST, VST3 |
| macOS | VST, VST3, AU |
| Linux | VST, VST3 |

---

## Processing audio from other applications (Windows)

Running plugins on your **microphone or audio interface** works out of the box:
select it as the input and you are done.

Running plugins on **audio from other applications** — a browser, a game, a media
player — needs one extra piece, and it is worth being precise about why.

Windows *can* capture another application's playback: WASAPI has supported
loopback since Vista, and Windows 10 2004 added a per-process version. **JUCE does
not expose either.** So the limitation is in the framework Light Host is built on,
not in the operating system, and a virtual audio device is the way around it
today. If that ever changes, it is tracked in [BACKLOG.md](BACKLOG.md).

The usual choice is **[VB-CABLE](https://vb-audio.com/Cable/)** (donationware;
needs admin rights and a reboot). Its naming trips up nearly everyone, because the
names are from the cable's point of view rather than yours:

| VB-CABLE calls it | It actually is | You will find it in |
|---|---|---|
| **CABLE Input** | a playback device — audio goes *into* the cable here | your **Output** list |
| **CABLE Output** | a recording device — audio comes *out* of the cable here | your **Input** list |

So to process system audio:

1. Set **CABLE Input** as the Windows playback device (or per-app in Volume Mixer).
2. In Light Host, choose **CABLE Output** as the input and your real speakers as
   the output.

Audio then flows: application → CABLE Input → Light Host → your plugins → your
speakers.

To send your *processed microphone* to other apps, run it the other way: real
microphone in, **CABLE Input** out, and point Teams, Discord or OBS at **CABLE
Output**.

> **Check the Communications role.** Windows keeps a separate default recording
> device for "Communications", and Teams, Zoom and most softphones use it. If it
> still points at your raw microphone, those apps bypass your entire chain.

Light Host does not bundle, download or install VB-CABLE. It only notices whether
a virtual input exists, and offers a **Get VB-CABLE** button that opens the
vendor's page. VoiceMeeter and other virtual audio drivers work equally well.

---

## Lanes, trims and delay compensation

Every plugin sits on a **lane** (0–3). Plugins on the same lane run in series;
lanes run in parallel and sum at the output. One lane is an ordinary serial chain,
which is what most setups want.

**Delay compensation is automatic** and done by JUCE's `AudioProcessorGraph`: it
accumulates each node's reported latency along every path, takes the maximum
across paths feeding a node, and delays the shorter ones. All lanes arrive
sample-aligned, and the host reports the slowest lane as its own latency.
Bypassed plugins keep their latency, which is correct — a plugin that reports
latency must produce the same latency when bypassed, so bypassing one does not
shift its lane in time.

**Preferences shows what it costs.** Under Device Settings, the `Latency` row
reads `X ms plugins + Y ms device = Z ms`. The device figure is its input and
output latency added together, and the total is the one you actually experience.
It updates when a plugin re-declares its latency, so switching a plugin's
latency mode shows the cost at the moment you change it. Note that `Buffer Size`
above it is the *device* block size and not the answer: a 10 ms buffer sitting
under a 94 ms plugin chain is a number that misleads if it is the only one on
screen — which is the pair of rows in the shot below.

<p align="center">
  <img src="docs/images/device-settings-zoomin.png" alt="The Device Settings section: Device API set to Windows Audio, sample rate 48 kHz, buffer size 480 samples at 10.0 ms, and beneath them the latency row reading 94.0 ms plugins plus 20.0 ms device equals 114.0 ms" width="520">
</p>

It is a readout, not a setting. Plugin latency is inherent to the plugins, and on
a live monitoring path there is nothing for the host to compensate — the only
levers are which plugins you load and their own latency modes.

**Lane trims** exist because parallel lanes fed the same input sum coherently:
four lanes of similar material arrive about 12 dB hot, two lanes about 6 dB. Each
lane gets a fader from mute to +12 dB, unity by default. They apply as you drag —
a level control you cannot hear while moving is not much use — and are written to
disk when you let go. Double-click to return to unity. Each trim ramps over 20 ms
so a change does not click.

A trim belongs to the lane, not the plugins on it. Empty lane 2 and its trim stays
put for whatever you add next.

---

## Seeing your own signal

Preferences shows a meter under the input device and another under the output,
each with a peak reading in dBFS and a **CLIP** badge.

**The badge latches.** It stays lit until you click it, rather than fading once
the level comes back down.

<p align="center">
  <img src="docs/images/clip-01.png" alt="The input row with the CLIP badge lit red, an arrow pointing at it, and the peak reading showing -49.1 dBFS — the badge still latched although the level has fallen far below full scale" width="520">
  <br>
  <img src="docs/images/clip-02.png" alt="The same input row after the CLIP badge has been clicked: the badge is dark again and the peak reads -44.8 dBFS" width="520">
  <br>
  <em>Top: the badge still lit at −49.1 dBFS, long after the peak that set it.
  Bottom: the same row after clicking the badge to clear it.</em>
</p>

That is deliberate: this feature exists because a microphone sat at +30 dB
hitting full scale for hours, the application said nothing about it, and the
first sign of trouble was other people on the call saying the audio was
unusable. A warning that had cleared itself by the time anyone looked would have
been the same as no warning.

If the input badge lights, the fix is upstream of Light Host — lower the Windows
input level for that device. Clipping happens in the converter, before any plugin
sees a sample, so nothing in the chain can undo it.

### The signal view

`Signal view` in the AUDIO CHAIN header opens a column to the right of the
window: the input, one row per plugin, then the output. Each row shows peak, RMS,
and **the change from the row above**.

<p align="center">
  <img src="docs/images/signal-view.png" alt="The Preferences window with the signal view column open on the right, showing a bar and peak and RMS figures for the input, for each of the three plugins in turn, and for the output, with each row's change from the row above reading +0.2 dB, -4.5 dB, +5.4 dB and +0.0 dB" width="720">
</p>

That last column is the useful one. It answers "which plugin is doing that?"
directly — a compressor adding 7 dB of make-up gain shows up as `+7.0 dB` on its
own row, which is otherwise a question that takes an offline render and a script
to answer.

Rows that do not fit the window are counted in the view's header rather than
drawn, because a meter you cannot see reads exactly like one showing silence.
Make the window taller to see them.

### What it costs while you are not looking

Light Host is tray-resident and its graph runs whenever an audio device is open,
so "idle" still means every block is being processed. Metering is therefore split
by what each part is for:

| | When it runs | Cost |
|---|---|---|
| Peak, on the two device meters | Always | A SIMD min/max scan — about 96k samples per second at 48 kHz stereo, several floats per instruction |
| RMS | Only while a meter is visible | A per-sample sum of squares |
| Per-plugin probes | Only while the signal view is open | Nothing at all when it is closed — the probes are removed from the graph |

With the window shut, the clip warning still works and everything else is off.
Closing the signal view returns the graph to exactly what it was before the
feature existed, which a test asserts rather than assumes.

### How this was designed

[`docs/mockups/level-meters.html`](docs/mockups/level-meters.html) is the mockup
these were chosen from: four options drawn at the real window width, including
the one that was declined and the panel showing the clipping incident that
prompted the whole feature. Open it in a browser. Its four signal colours are
pinned to the source and annotated with where each one lives, so a colour change
that leaves the mockup behind is visible rather than silent.

---

## Testing your chain on a file

Judging a microphone chain by talking into it tells you very little. You cannot
say the same sentence twice, so you cannot A/B two settings against identical
audio, and level differences alone will convince you one of them is better. So
the host can push a file through the chain you have configured and write the
result.

**In the app:** *Preferences → Chain Test*. It asks for an input file and an
output folder, renders, then reveals the file. The window stays usable while it
works.

**From the command line:**

```
"Light Host" --render input.wav output.wav
```

It reports what it did — plugins active and bypassed, connection count, declared
latency, frames written — along with every active plugin's parameter values, read
back out of the plugin *after* its saved state was restored. A measurement is
only worth as much as the settings it ran under, and reading those out of the
plugin beats reading them off a screenshot.

### It renders in real time, deliberately

A render is paced so a minute of audio takes a minute. That looks wasteful and is
not.

Plugins that run neural inference do it on a worker thread. Rendered at thirty
times real time that thread never keeps up, the plugin falls back to passing the
dry signal through, and the render prints `RENDER OK` having measured nothing.
This is not hypothetical. A DeepFilterNet-based denoiser measured **0.55 dB** of
noise reduction that way, and on a quieter take it appeared to *raise* the noise
floor by 8.5 dB. Paced to real time, the same plugin at the same settings removed
**24.8 dB** of keyboard noise for 1.7 dB of speech level. Every conclusion drawn
from the unpaced numbers was wrong, and none of them looked wrong at the time.

`--fast` skips pacing for plugins known to work synchronously and is roughly
thirty times quicker. Every render prints a `realtime factor` and says when it
could not keep pace — phrased as an observation, not a verdict, because a
synchronous plugin was measured falling behind on 998 of 10,224 blocks while
producing bit-identical output.

### Comparing plugins and settings

| Flag | What it does |
|---|---|
| `--param "NAME=VALUE"` | Sets a parameter using the plugin's own text parser |
| `--param "NAME@0.25"` | Sets the raw normalised position instead |
| `--chain "A,B"` | Renders through these plugins instead of the saved chain |
| `--fast` | Skips real-time pacing |

`--param` and `--chain` are both repeatable, and the repeats append in order.

`--chain` looks names up in the scanned plugin list rather than the saved chain,
so a candidate can be measured against your current setup without being added to
it first — comparing two plugins should not require reconfiguring the thing you
are measuring.

```
"Light Host" --render take.wav out.wav --chain "Alt Denoiser" --param "Attenuation Limit=0"
```

### Scanning for plugins without opening a window

`--chain` resolves names against the scanned plugin list, which is empty on a
machine that has never opened *Edit Plugins* from the tray. `--scan` fills it
from the command line, so a render — or the regression gate that depends on one
— works on a fresh checkout without anyone clicking anything.

```
"Light Host" --scan
"Light Host" --scan --scan-path "C:\Program Files\Common Files\VST2"
```

| Flag | What it does |
|------|--------------|
| `--scan` | Scans every format's default locations plus any folder you added in *Edit Plugins*, and writes the result to the settings file |
| `--scan-path DIR` | Also scans `DIR`. Repeatable. Added to the defaults, never instead of them |

It prints each file as it tries it, then a count, and lists anything that looked
like a plugin and would not load — which is the single most useful thing a scan
can tell you and the thing a scan used to throw away.

`--scan-path` is not optional on every machine. JUCE's default VST2 locations do
not include `%COMMONPROGRAMFILES%\VST2`, which is where the ReaPlugs installer
puts ReaEQ, so that whole folder is invisible to a bare `--scan`.

The list is written after each format finishes rather than once at the end.
Scanning loads every plugin it finds into this process, so a plugin that
hard-crashes the scan takes Light Host down with it -- and a single write at the
end would lose everything that run had already found. It skips the offender next
time (that is what the crash list is for), but only if the work before it
survived.

`--scan` is the only command line here that writes to your settings file, and
the scanned list is all it writes. A render writes nothing back: not settings,
not plugin state, not node ids. It is safe to run against a live configuration.
Neither opens an audio device or shows a tray icon, so neither disturbs a
running instance.

---

## Where your settings live

| | Path |
|---|---|
| Windows | `%APPDATA%\Light Host\` |
| macOS | `~/Library/Preferences/` and `~/Library/Logs/Light Host/` |
| Linux | `~/.config/Light Host/` |

Two things live there:

- **`Light Host.settings`** — your chain, order, lanes, bypass states, trims and
  device selection. Readable XML, and small: about twelve kilobytes.
- **`Light Host.state/`** — one gzip-compressed file per plugin, holding that
  plugin's own saved settings.

Plugin state used to live base64-encoded inside the settings file. On a real chain
of five plugins that made the file 7.5 MB, of which 99.84% was plugin state — and
because `juce::PropertiesFile` rewrites the whole document on every save, toggling
one bypass wrote seven and a half megabytes. Splitting them out means a bypass
toggle writes twelve kilobytes, and only the plugin that actually changed is
written at all.

Upgrading from an earlier version migrates this once, on first launch. The file is
written, read back and verified *before* the old copy is dropped, so a failed
migration leaves your presets where they were rather than losing them.

**Worth backing up.** `Light Host.state/` holds anything you have trained or
tuned inside a plugin, and some of that is not quickly reproducible.

> The tray menu has **Delete Plugin States**, above **Quit** and separated from
> it. It erases the saved settings of every plugin in the chain, so it names
> them and asks first.

---

## Running more than one instance

```bash
Light Host -multi-instance=NAME
```

Starts a second, independent copy with its own settings file and its own state
directory — so you can run one chain on a microphone and another on system audio
without them fighting over one configuration.

The name goes into a filename, so it is reduced to letters, digits, hyphens and
underscores and capped at 32 characters. A name containing a path separator would
otherwise write outside the settings folder; one containing a character the
filesystem rejects would produce a file that could never be saved. A name that
sanitises to nothing becomes `instance` rather than silently falling back to your
main configuration.

**Without that flag, a second launch does nothing.** It hands its command line to
the copy already running and exits — which is normal single-instance behaviour,
but it used to be completely silent, so launching a newly built copy looked like
nothing had happened while you carried on inspecting the old one. It now says so,
in the log and in the status row, and brings Preferences to front.

---

## When something goes wrong

**Hover the tray icon.** When something has failed, the tooltip says what: a
plugin that would not load, a plugin that refused its saved settings, an audio
device that would not open, a settings file that could not be written. The same
message sits at the top of Preferences with a **Show Log** button beside it,
because a tooltip is only found by someone who already suspects something.

**The log** is a single rotating file, trimmed to 256 KB on every launch:

| | Path |
|---|---|
| Windows | `%APPDATA%\Light Host\LightHost.log` |
| macOS | `~/Library/Logs/Light Host/LightHost.log` |
| Linux | `~/.config/Light Host/LightHost.log` |

It records startup and shutdown, audio configuration at startup and on every
device change (driver, device names, active-versus-total channels, sample rate,
buffer size), plugin loads and failures, graph rewiring, Preferences activity,
Apply operations, and every exception caught around plugin state, plugin-list
mutation and device switching. Each run starts with a
`==== Light Host X.Y.Z starting at <time> ====` banner.

If the host disappears without a message, the log is the first place to look.

---

## Known limitations

- **Stereo-focused routing.** Two channels in, two channels out.
- **Level metering only.** Input, output and per-plugin levels with a clip
  indicator; no spectrum, phase or loudness display.
- **No MIDI, no side-chain, no recording, no undo.**
- **Plugins run in-process.** A plugin that crashes takes the host with it. Real
  sandboxing is a different application, not a fix.
- **Nothing is code-signed.** Windows shows a SmartScreen warning on first run;
  macOS refuses the app until you right-click → Open or run
  `xattr -dr com.apple.quarantine "Light Host.app"`.
- **Renaming an audio device** after selecting it orphans the selection,
  because JUCE stores the device by display name rather than by a stable id.
  Light Host then reports which device went missing and what is being used
  instead, in the tray tooltip and the Preferences status row, but it cannot
  follow the rename automatically — re-select the device. Storing a stable
  endpoint id would need Windows-only code outside JUCE; assessed in
  [DECISIONS.md](DECISIONS.md).
- **No echo cancellation.** If your speakers are audible to your microphone,
  Light Host cannot remove them: cancelling an echo requires knowing what was
  played, and Light Host has no access to that. Use headphones, or leave your
  conferencing application's echo cancellation switched on — it *does* know.
  Assessed in detail in [DECISIONS.md](DECISIONS.md).

Known bugs, as distinct from missing features, are in [BACKLOG.md](BACKLOG.md).
Things deliberately not built, and why, are in [DECISIONS.md](DECISIONS.md).

---

## Building from source

Light Host vendors JUCE 9.0.2 and the VST2 SDK, so there is nothing to fetch
beyond a compiler and CMake.

### Windows

- Visual Studio 2026, Windows SDK `10.0.26100.0`
- CMake `4.2+` (required by the Visual Studio 18 2026 generator)

```bash
cmake --preset release
cmake --build --preset release
```

The C runtime is linked statically, so the executable needs no Visual C++
redistributable and can be copied to another machine as a single file.

### macOS

- Xcode command line tools, Ninja, CMake `3.28+`

Builds universal (`arm64;x86_64`) with a macOS 11 floor, so one binary runs on
Apple Silicon and Intel. Use `-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster local
build.

### Linux / WSL

- GCC or Clang, Ninja, CMake `3.28+`, `pkg-config`

The package list is [`tools/linux-build-deps.txt`](tools/linux-build-deps.txt),
and it is not repeated here on purpose. It used to be, and the copy here was
already short by seven packages including the two mesa ones — which is exactly
the drift that put a missing `libxi-dev` into CI in the first place. Both
workflows and the container script read that one file, so this reads it too:

```bash
sudo apt update
grep -vE '^[[:space:]]*(#|$)' tools/linux-build-deps.txt \
  | xargs sudo apt install -y --no-install-recommends
cmake --preset ninja-release
cmake --build build/ninja-release -j"$(nproc)"
```

`libxi-dev` is needed by JUCE 9 and was not by JUCE 8; without it the build fails
on `X11/extensions/XInput2.h`.

### Building for Linux from Windows

[`tools/build-linux-docker.sh`](tools/build-linux-docker.sh) configures, builds
and runs the whole test suite in `ubuntu:24.04`, using the same dependency list
CI does:

```bash
tools/build-linux-docker.sh            # configure, build, test
tools/build-linux-docker.sh --build    # stop after the build
tools/build-linux-docker.sh --shell    # a shell in the container
```

It needs Docker. It exists because GCC is much fussier than MSVC and warnings
are errors here, so without it the first evidence that Linux still compiles
arrives from CI, after the push. macOS has no local equivalent.

### Presets

`default` · `release` · `ninja-debug` · `ninja-release` · `ninja-relwithdebinfo` ·
`clang-debug` · `clang-release` · `mingw-release`

`clang-release` builds on Windows, which it did not before 5.3.0: the MSVC-only
flags were guarded on `WIN32` rather than on `MSVC`, and clang++ reads `/utf-8`
as a missing input file, so it died on the first translation unit. It needs LLVM
on `PATH`, and it is the cheapest way to catch an MSVC-only construct before CI
finds it.

The built application lands in
`build/<preset>/LightHost_artefacts/<config>/Light Host[.exe]`.

---

## Tests

Unit tests build by default (`-DLIGHTHOST_BUILD_TESTS=OFF` to skip) and run under
CTest. They use `juce::UnitTestRunner`, so there is no third-party dependency. No
audio device and no real plugin are needed anywhere.

Two CTest entries, because one group needs more than the other. `unit` needs
nothing and runs on all three platforms. `unit-gui` constructs real editor
windows, so it needs a display and is registered on Windows and macOS only —
JUCE's X11 backend cannot create a window from a console application, which is
what the test binary is. CMake prints a notice at configure time rather than
skipping it quietly, and Linux window creation is covered by the smoke test
below, which runs the real application under `xvfb`.

```bash
cmake --build --preset release
ctest --test-dir build/release -C Release --output-on-failure
```

What they cover:

- **Lane alignment**, rendered through a real `AudioProcessorGraph` with stub
  processors that declare latency — passthrough, one lane, two lanes of differing
  latency, four chained lanes whose latencies cross block boundaries, equal-latency
  lanes, and a bypassed lane. Each asserts one impulse on one sample and the
  correct reported host latency.
- **Chain wiring**, as a pure function over node facts with no graph at all:
  serial order, parallel lanes, mono-to-stereo duplication, mono destinations,
  nodes that cannot carry audio being wired around, corrupt lane indices being
  clamped, and the two invariants live rewiring depends on.
- **Lane trims**, rendered rather than reasoned about: unity is transparent, −6 dB
  halves, the range clamps, a mid-stream change ramps instead of stepping, and
  re-preparing does not fade the lane in.
- **Chain settings**: identity surviving a plugin update and a rename while still
  telling apart two plugins inside one shell file, staged writes invisible until
  commit and undone by rollback, an erase leaving no field behind, ordering ties
  broken deterministically, and the one-shot migration from the old key format.
- **Staged-chain reconcile**: what becomes of an edit you have not applied when
  the committed chain changes underneath it. Nothing staged and the committed
  chain is taken verbatim; with an edit pending, a staged addition survives, a
  staged deletion is not resurrected, a plugin added or removed elsewhere is
  honoured, and staged bypass, lane and order win. Duplicate identities are
  matched positionally rather than all onto the first, a deletion and an arrival
  are told apart at the tail, and the "chain changed elsewhere" message is gated
  on the list having moved by itself, not on an edit merely being pending.
- **Plugin state**: a good blob round-trips, an empty one reports "nothing saved"
  rather than failure, a plugin that throws out of `setStateInformation` reports
  failure without letting the exception escape, and the resulting rule — never
  save a node whose restore failed — leaves the stored preset intact.
- **The state vault**: round trips, that compression is actually running, and the
  three ways a file can be unusable (absent, truncated, written by a later
  version) all reading as "nothing stored" rather than as garbage handed to a
  plugin.
- **Instance names**: every escape attempt a `-multi-instance` name could make.
- **Signal metering**: that a peak survives the blocks after it, that reading is
  not destructive so two components watching one meter agree, that RMS is only
  computed while something is watching, and that a probe passes audio through
  bit-unchanged with no declared latency.
- **Reserved node ids**: the published numbers, which are an on-disk contract;
  that the lane, probe and IO bands cannot overlap; and that an out-of-range lane
  or probe index is clamped rather than wrapped into another band.
- **Audio device substitution**: that an opened device which is not the one
  requested is reported and names both; that the recorded request wins over the
  stored `DEVICESETUP` and a missing record falls back to it; and that it stays
  quiet when it should, including a capitalisation-only rename, an input-only
  setup, and a first run that has neither source to compare.
- **Confirmation button order**: both platform rules from one build, because they
  disagree about which index a dismissed dialog reports and the destructive
  button must sit on neither.
- **Plugin editor windows**: that an editor constructor which throws yields no
  window instead of taking the process down, and (in `unit-gui`) that asking
  twice for a plugin with no native editor reuses one window.
- **The chain list**: that bypass and lane travel with their plugin through a
  reorder, a delete and an add, the invariant one row vector replaced three
  parallel ones to get; that an identity already staged is refused and fires no
  change; that both asynchronous row menus resolve by identity the row they were
  opened on, so a chain changing while a menu is open cannot redirect a delete
  or a lane onto another plugin; the drag-insertion arithmetic at both ends and
  past them; and the press contract — armed on press, fired on release,
  cancelled when dragged off — including the gap above the first row that used
  to arm a drag on row 0. Headless, so it is in `unit`, not `unit-gui`.
- **The command-line parsers**: each answer the offline renderer takes off a
  command line, including that a path the line does not carry is left alone
  rather than cleared, and that nothing on it can switch pacing off by accident.
- **Scan path precedence**: a format's own defaults first, a folder remembered
  from *Edit Plugins* next, `--scan-path` last, and a folder that is not there
  skipped rather than searched. Plus the write at the end of each format's
  scan: that the list lands under the key the application reads, and that a
  write which cannot reach disk is reported rather than swallowed. The scan
  itself is not covered, because it loads whatever plugins are installed on the
  machine running the tests.
- **The self-test's own checks**: that every startup and shutdown marker is
  checked one at a time rather than as a set, and that the assertion a smoke
  test is allowed to ignore is one named file and not a topic.
- **Settings keys**: the literal strings, which are an on-disk contract written
  by every released version, so the test exists to fail if someone improves a
  name.
- Plus the sample-rate correction policy and the status sink.

### Startup smoke test

```bash
"Light Host" -self-test
```

Runs the real application — real device manager, real graph, real tray icon, real
message loop, real teardown — inspects its own log, prints `SELF-TEST PASS` or a
list of failures, and exits non-zero on any problem. Settings and log go to a
throwaway folder unique to the run, so it can never read or overwrite your real
configuration; the folder is deleted on success and kept on failure.

Every run starts from a seeded one-plugin chain written in the old settings
format, so it covers the one-shot migration, the move of plugin state into its own
file, a plugin that cannot be instantiated being reported and skipped rather than
stalling the load, and the rule that a plugin which never loaded keeps its saved
state. The seeded plugin matches no registered format, so none of this needs a
real plugin on disk.

CTest registers three: a first run, an identical repeat, and one with
`-multi-instance=`. They are labelled `smoke`, so `ctest -L unit` and
`ctest -L smoke` can be run separately.

This is the only automated check that exercises the destructor ordering in
`~IconMenu` that prevents a shutdown crash — and it is not a substitute for
testing that by hand. On a machine with no audio device no callback thread ever
starts, so the race the ordering guards against cannot occur. It proves the
ordering code runs cleanly, not that the race is fixed.

On Linux this needs a display: JUCE does not degrade gracefully without one, so
CMake registers the smoke tests only when `xvfb-run` is present.

### The two checks CTest cannot run

Neither is registered, because one needs a third-party plugin installed and the
other needs the network. Both are run by hand before a release.

```bash
tools/render-regression.sh capture <input.wav>   # once per machine
tools/render-regression.sh check   <input.wav>
```

Renders a fixed input through a deterministic ReaEQ chain and compares a sha256
against a stored baseline. Almost nothing this project changes is supposed to
alter a sample, and "inaudible" is far easier to get wrong than to verify. The
baseline and the input stay local: the hash depends on which plugins are
installed, and the input is a voice recording in a public repository. The render
is paced, so a 20 second input takes 20 seconds. It needs a scanned plugin list,
which is what `--scan` is for.

```bash
tools/ci-status.sh [ref]
```

Answers whether CI actually passed on a commit. It exists because
`gh run watch --exit-status` returned 0 for two runs that had failed. Exit 0
needs the run's `conclusion` to be exactly `success` and every job to agree; no
run at all is a failure, because no evidence is not success.

### The rest of `tools/`

[`tools/README.md`](tools/README.md) indexes all of it in one line each: the
build and CI scripts above, and the operator tooling that measured the real
microphone chain -- capture, level analysis, WASAPI endpoint and role listing,
capture-level read and write, and idle CPU. None of it is part of the
application or run by CTest, and most of it needs a real audio device, which is
why none of it is a gate.

---

## Project layout

```text
.
├── Source/          Application source
├── Tests/           Unit tests (juce::UnitTestRunner)
├── Resources/       Icons and binary resources
├── docs/images/     Screenshots used by this README
├── docs/mockups/    Design mockups, annotated with what shipped
├── tools/           Build, CI and audio scripts -- see tools/README.md
├── Utilities/       Helper scripts
├── lib/             Vendored JUCE 9.0.2 + VST2 SDK
├── .github/         CI and release workflows
├── CMakeLists.txt   Build definition (the only place the version lives)
├── CMakePresets.json
├── CHANGELOG.md     What changed, and why
├── BACKLOG.md       Known bugs and remaining work
├── DECISIONS.md     What was considered and not built, and why
└── RELEASING.md     How a release is cut
```

---

## Licence

Light Host's own source is **GPLv2-or-later**, inherited from Rolando Islas's
original Light Host.

**The application as a whole is conveyed under AGPLv3.** It links JUCE, which is
AGPLv3 unless you hold a commercial JUCE licence, and this project does not. It
also uses the Steinberg ASIO SDK under its GPLv3 option. GPLv2-or-later can be
taken up to GPLv3, and GPLv3 and AGPLv3 may be combined, so the result is
distributable — but anyone receiving a Light Host binary receives AGPLv3 terms.

One exception: the Steinberg VST 2.4 SDK headers in `lib/vstsdk2.4` are governed
by Steinberg's own agreement, which is not GPL-compatible and is not covered by
the grant above. VST2 hosting is on by default (`JUCE_PLUGINHOST_VST=1`); building
with it off and removing that directory gives a tree without the exception.

See [`license`](license) for the full picture including the VST2 exception,
[`agpl-3.0.txt`](agpl-3.0.txt), [`gpl.txt`](gpl.txt), and
[`third_party`](third_party) for every bundled component.
