#!/usr/bin/env bash
# Renders a fixed input through a DETERMINISTIC reference chain and compares the
# result against a stored baseline hash.
#
#   tools/render-regression.sh capture <input.wav> [chain]
#   tools/render-regression.sh check   <input.wav> [chain]
#
# Why a byte-for-byte comparison is the right test: almost nothing this project
# changes is supposed to alter a sample. Metering probes, node-id refactors,
# cache-lifetime changes and settings-key work all have to be inaudible, and
# "inaudible" is much easier to get wrong than to verify. A hash either matches
# or it does not, which is a better gate than listening and deciding it sounds
# the same.
#
# WHY THE CHAIN IS OVERRIDDEN, AND WHY THIS IS NOT OPTIONAL
#
# The live chain CANNOT be used as a byte gate. Measured 2026-09-16, four paced
# renders of one input: ReaEQ produced one hash 4/4 times; Salvor produced two
# different hashes. Salvor does its inference on a worker thread, and even when
# the render keeps pace (realtime factor 1.000, no blocks behind), the worker
# delivers results on slightly different block boundaries from run to run. The
# full Salvor + smartChain chain inherits that.
#
# So a gate built on the live chain fails at random and teaches you to ignore
# it, which is worse than having no gate. The default chain here is ReaEQ:
# plain deterministic DSP with no worker thread. It still exercises everything
# this project actually changes -- graph construction, connection diffing, lane
# trims, probe transparency, state restore, node ids and the sorted-chain cache
# -- because those live in the host, not in the plugin.
#
# To check the real chain, listen to it. That is a different test and this
# script is not it.
#
# The render is PACED (real time), which is a correctness requirement rather
# than a performance accident: an unpaced render starves a worker-thread plugin
# and produces a different, wrong result. See DECISIONS.md. A 20 s input takes
# 20 s; do not add --fast to speed it up.
set -euo pipefail

mode=${1:-}
input=${2:-}
chain=${3:-reaeq}

here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
baseline="$here/tools/render-regression.sha256"

# Where the build can be, in the order it is looked for. JUCE puts products
# under <target>_artefacts/<CONFIG>/ for the single-config Ninja presets as well
# as for the multi-config MSVC one, so what differs between platforms is the
# build directory and the file at the end of it, not the layout. macOS produces
# a bundle, and --render has to be given the executable inside it; that
# candidate comes before the bare name because only one of the two exists.
candidates=(
    "$here/build/release/LightHost_artefacts/Release/Light Host.exe"
    "$here/build/ninja-release/LightHost_artefacts/Release/Light Host.exe"
    "$here/build/ninja-release/LightHost_artefacts/Release/Light Host.app/Contents/MacOS/Light Host"
    "$here/build/ninja-release/LightHost_artefacts/Release/Light Host"
)

if [[ -z "$mode" || -z "$input" ]]; then
    echo "usage: tools/render-regression.sh {capture|check} <input.wav> [chain]" >&2
    exit 2
fi

exe=""

for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
        exe="$candidate"
        break
    fi
done

# Every path, not just the one for this platform: the usual reason for landing
# here is having built a different preset from the one being looked for, and
# that is only obvious if the list is in front of you.
if [[ -z "$exe" ]]; then
    echo "no build found. Tried:" >&2

    for candidate in "${candidates[@]}"; do
        echo "  $candidate" >&2
    done

    echo "build first: cmake --build build/release --config Release   (Windows, VS)" >&2
    echo "         or: cmake --build build/ninja-release              (Linux, macOS)" >&2
    exit 2
fi

if [[ ! -f "$input" ]]; then
    echo "no such input: $input" >&2
    exit 2
fi

# The extension has to be added by moving the file, not by concatenating onto
# the command substitution: "$(mktemp ...).wav" leaves mktemp's own file behind
# and traps a path that was never created.
tmp=$(mktemp -t render-regression-XXXXXX)
out="$tmp.wav"
mv "$tmp" "$out"
trap 'rm -f "$tmp" "$out"' EXIT

echo "rendering through '$chain' (paced, so this runs in real time)..."
"$exe" --render "$input" "$out" --chain "$chain"

if [[ ! -f "$out" ]]; then
    echo "REGRESSION TOOL FAIL: the render produced no file" >&2
    exit 1
fi

# sha256sum is coreutils, which macOS does not ship; shasum is perl and is on
# macOS, Linux and git-for-Windows alike. Both are accepted rather than picking
# one, because the hash in the baseline file has to be the same string whichever
# of them produced it -- and it is: they print the same digest, only the flags
# differ.
sha256_of()
{
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum > /dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        echo "no sha256 tool: install coreutils for sha256sum, or use a shell with shasum" >&2
        return 2
    fi
}

hash=$(sha256_of "$out")

case "$mode" in
    capture)
        echo "$hash" > "$baseline"
        echo "baseline recorded: $hash"
        echo "(stored in tools/render-regression.sha256)"
        ;;
    check)
        if [[ ! -f "$baseline" ]]; then
            echo "no baseline yet; run 'capture' first" >&2
            exit 2
        fi

        want=$(cat "$baseline")

        if [[ "$hash" == "$want" ]]; then
            echo "REGRESSION OK: $hash"
        else
            echo "REGRESSION FAIL"
            echo "  expected: $want"
            echo "  actual  : $hash"
            echo ""
            echo "A sample changed. If that was deliberate, re-capture the"
            echo "baseline in the same commit and say why in the message."
            exit 1
        fi
        ;;
    *)
        echo "unknown mode: $mode (want capture or check)" >&2
        exit 2
        ;;
esac
