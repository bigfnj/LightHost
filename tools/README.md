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

## Measuring a real microphone chain

These are how the clipping and denoiser figures quoted in the README and
CHANGELOG were arrived at. They need a real capture device, so none of them can
run in CI.

| Script | What it does |
|---|---|
| [`voice-headroom.sh`](voice-headroom.sh) | Records one 18 s take that is quiet first and speech second, so the two can be measured apart. Needs `ffmpeg` and a dshow device name |
| [`analyse-voice-headroom.py`](analyse-voice-headroom.py) | Splits that take at the instructed boundary and reports peak, RMS and clipped-sample counts for each half |
| [`compare-capture.py`](compare-capture.py) | Compares a raw-microphone capture against a virtual-cable capture, to tell "chain broken" from "chain bypassed" |
| [`audio-endpoints.ps1`](audio-endpoints.ps1) | Lists every active endpoint and which of the three Windows default roles it holds -- the Communications role is why a correctly wired chain can still be bypassed |
| [`show-capture-levels.ps1`](show-capture-levels.ps1) | Reads every capture endpoint's level as both a slider percentage and dB, with its range and mute state |
| [`set-capture-level.ps1`](set-capture-level.ps1) | Sets one named endpoint's capture level in dB and reads it back |
| [`measure-idle-cpu.ps1`](measure-idle-cpu.ps1) | Measures CPU used while tray-resident with no window open, because "costs nothing while idle" is a claim about cost |

## Dependencies

`voice-headroom.sh` needs `ffmpeg`. The two Python scripts need `numpy`.
The four `.ps1` files are Windows-only and compile a small block of C# at run
time to reach the WASAPI endpoint interfaces, which PowerShell does not expose;
none of them needs an elevated shell.
