# tools/

Operator and maintainer scripts. None of them is part of the application, none
is run by `ctest`, and nothing in the build depends on any of them.

This index exists because eight of the files here were referenced by no document
at all. Seven of the eight carried a header comment explaining themselves, so
they were never clutter -- but a script nothing points at is as good as a script
that is not there. The eighth, `show-capture-levels.ps1`, opened on `$src = @'`
and explained nothing; it has a header now too.

Everything below is one line. Where a script is documented properly somewhere
else, the link is the documentation and this table is only the signpost.

## Build and CI

| Script | What it does |
|---|---|
| [`build-linux-docker.sh`](build-linux-docker.sh) | Configures, builds and tests for Linux in `ubuntu:24.04`. See [README](../README.md#building-for-linux-from-windows) |
| [`Dockerfile.linux-build`](Dockerfile.linux-build) | The image `build-linux-docker.sh` builds. Edit this, not the script, to change the Linux toolchain |
| [`linux-build-deps.txt`](linux-build-deps.txt) | The one dependency list CI, the container and the README all read. See [README](../README.md#linux--wsl) |
| [`ci-status.sh`](ci-status.sh) | Answers whether CI actually passed on a commit, which `gh run watch` does not. See [README](../README.md#the-two-checks-ctest-cannot-run) |
| [`update-juce.sh`](update-juce.sh) | Replaces the vendored JUCE tree with a tagged upstream release, and re-runs the standing checks a bump can invalidate |

## Audio regression

| Script | What it does |
|---|---|
| [`render-regression.sh`](render-regression.sh) | Renders a fixed input through a deterministic chain and compares a sha256 against a stored baseline. See [README](../README.md#the-two-checks-ctest-cannot-run) |
| `render-regression.sha256` | The baseline hash `render-regression.sh` writes and reads. Gitignored and per-machine, so it is absent from a fresh clone until you run `capture` -- the digest depends on which plugin build is installed, so one committed value would be wrong for everybody else |

## Measuring a real microphone chain

These are how the clipping and denoiser figures quoted in the README and
CHANGELOG were arrived at. They need a real capture device, or a take recorded
from one, so none of them can run in CI.

| Script | What it does |
|---|---|
| [`voice-headroom.sh`](voice-headroom.sh) | Records one 18 s take that is quiet first and speech second, so the two can be measured apart. Needs `ffmpeg` and a dshow device name |
| [`analyse-voice-headroom.py`](analyse-voice-headroom.py) | Splits that take at the instructed boundary and reports peak, RMS and clipped-sample counts for each half |
| [`compare-capture.py`](compare-capture.py) | Compares a raw-microphone capture against a virtual-cable capture, to tell "chain broken" from "chain bypassed" |
| [`audio-endpoints.ps1`](audio-endpoints.ps1) | Lists every active endpoint and which of the three Windows default roles it holds -- the Communications role is why a correctly wired chain can still be bypassed |
| [`show-capture-levels.ps1`](show-capture-levels.ps1) | Reads every capture endpoint's level as both a slider percentage and dB, with its range and mute state |
| [`set-capture-level.ps1`](set-capture-level.ps1) | Sets one named endpoint's capture level in dB and reads it back |
| [`measure-idle-cpu.ps1`](measure-idle-cpu.ps1) | Measures CPU used while tray-resident with no window open, because "costs nothing while idle" is a claim about cost |
| [`cpu-load.ps1`](cpu-load.ps1) | Spins a chosen number of CPU-bound threads for a bounded time, so "under load" is a dial setting and not whatever else the machine happened to be doing. Reports cores *delivered*, which on a busy box is well under the threads asked for |
| [`denoiser-ab.ps1`](denoiser-ab.ps1) | Sweeps each denoiser candidate against a dry reference at several load levels, repeats every combination, and reports medians and spread. Exists to fire or clear the reversal trigger recorded in [DECISIONS](../DECISIONS.md) for keeping Salvor over Alt Denoiser |
| [`score-sections.py`](score-sections.py) | Scores one render against a dry reference section by section, so noise removed and voice damaged stay two numbers rather than one sum. Borrows `load`, `db` and `stats` from `analyse-voice-headroom.py` so the two cannot drift apart |

## Dependencies

`voice-headroom.sh` needs `ffmpeg`. The three Python scripts need `numpy`. All
six `.ps1` files are Windows-only. Three of them -- `audio-endpoints.ps1`,
`show-capture-levels.ps1` and `set-capture-level.ps1` -- compile a small block
of C# at run time to reach the WASAPI endpoint interfaces, which PowerShell
does not expose; `measure-idle-cpu.ps1` and `cpu-load.ps1` need none, because a
process's `TotalProcessorTime` is already a PowerShell property and a runspace
pool is already a PowerShell type. None of them needs an elevated shell.

`denoiser-ab.ps1` needs more than any of the others, and checks for all of it
before spending an hour rather than after: a Release build to render through,
an interpreter that has `numpy` (it tries `-Python`, then `$env:TOOLBOX_PYTHON`,
then `python3` and `python`, and prints every one it tried), and an input take.
It will not invent a take. `tools/*.wav` is gitignored, so a fresh clone has
none and there is nothing to copy -- record one with `voice-headroom.sh` first.
