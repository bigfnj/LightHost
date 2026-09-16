#!/bin/bash
#
# Replace the vendored JUCE tree with a tagged upstream release.
#
#   tools/update-juce.sh 9.0.2
#
# JUCE lives at lib/juce as plain files rather than a submodule, so a bump is a
# wholesale directory replacement and a large, mechanical commit. That is the
# established pattern here -- git log -- lib/juce shows three prior bumps and no
# local patches, which is what makes replacing the tree safe. If anyone ever does
# patch the vendored sources, this script will silently discard it, so check that
# log before running.
#
# After this runs you MUST delete the build directory. JUCE generates the Windows
# version resource through juceaide at configure time; an incremental build over a
# changed JUCE relinks without complaint against stale generated files.
set -euo pipefail

VERSION="${1:?usage: tools/update-juce.sh <version>   e.g. 9.0.2}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

URL="https://github.com/juce-framework/JUCE/archive/refs/tags/${VERSION}.tar.gz"

echo "fetching JUCE ${VERSION}"
curl -fsSL "$URL" -o "$WORK/juce.tar.gz"

echo "extracting"
mkdir -p "$WORK/x"
tar -xzf "$WORK/juce.tar.gz" -C "$WORK/x"

SRC="$WORK/x/JUCE-${VERSION}"

# Refuse to proceed on an archive that does not look like JUCE, rather than
# emptying lib/juce and copying rubbish into it.
if [ ! -d "$SRC/modules/juce_core" ] || [ ! -f "$SRC/CMakeLists.txt" ]; then
  echo "archive does not look like a JUCE source tree: $SRC" >&2
  exit 1
fi

echo "replacing $ROOT/lib/juce"
rm -rf "$ROOT/lib/juce"
mkdir -p "$ROOT/lib/juce"
cp -R "$SRC/." "$ROOT/lib/juce/"

echo
echo "vendored version is now:"
grep -E "JUCE_(MAJOR_VERSION|MINOR_VERSION|BUILDNUMBER)" \
  "$ROOT/lib/juce/modules/juce_core/system/juce_StandardHeader.h"

# System-audio loopback capture: re-checked on every bump, because the answer can
# only ever change upstream. Processing audio from other applications needs a
# third-party virtual input device (VB-CABLE or similar) solely because JUCE's
# device layer does not expose WASAPI loopback -- Windows has supported it since
# Vista, so this is a framework gap, not a platform one. Nobody would think to
# look again, so the script looks for us. Today this finds nothing usable:
# WASAPIDeviceMode is shared, exclusive and sharedLowLatency, and the string does
# not appear in the module at all.
#
# A non-empty result means the feature request may have become buildable. Read the
# hits, then revisit the Deferred entry in BACKLOG.md, the "Acoustic echo
# cancellation" entry in DECISIONS.md (loopback is its prerequisite), and the hint
# text in Source/PreferencesWindow.cpp that tells users to install a virtual cable.
echo
echo "checking whether JUCE has gained system-audio loopback support:"
if grep -ri loopback "$ROOT/lib/juce/modules/juce_audio_devices"; then
  echo "  ^^ loopback now appears in juce_audio_devices -- see the system-audio"
  echo "     capture entry in DECISIONS.md, which this check exists to revisit"
else
  echo "  nothing (as expected) -- WASAPI loopback is still not exposed by JUCE"
fi

echo
echo "next: rm -rf build/release && cmake -S . --preset release && cmake --build build/release --config Release && ctest --test-dir build/release -C Release"
