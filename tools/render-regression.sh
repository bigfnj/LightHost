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
exe="$here/build/release/LightHost_artefacts/Release/Light Host.exe"
baseline="$here/tools/render-regression.sha256"

if [[ -z "$mode" || -z "$input" ]]; then
    echo "usage: tools/render-regression.sh {capture|check} <input.wav> [chain]" >&2
    exit 2
fi

if [[ ! -f "$exe" ]]; then
    echo "no build at: $exe" >&2
    echo "build first: cmake --build build/release --config Release" >&2
    exit 2
fi

if [[ ! -f "$input" ]]; then
    echo "no such input: $input" >&2
    exit 2
fi

out=$(mktemp -t render-regression-XXXXXX).wav
trap 'rm -f "$out"' EXIT

echo "rendering through '$chain' (paced, so this runs in real time)..."
"$exe" --render "$input" "$out" --chain "$chain"

if [[ ! -f "$out" ]]; then
    echo "REGRESSION TOOL FAIL: the render produced no file" >&2
    exit 1
fi

hash=$(sha256sum "$out" | cut -d' ' -f1)

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
