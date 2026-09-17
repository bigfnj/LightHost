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

version_header="$ROOT/lib/juce/modules/juce_core/system/juce_StandardHeader.h"

if [[ ! -f "$version_header" ]]; then
  echo "  CANNOT READ THE VERSION: $version_header is missing." >&2
  echo "  The old lib/juce has already been replaced, so the tree is on the new" >&2
  echo "  JUCE with no version recorded. Find where the version moved to." >&2
  exit 1
fi

grep -E "JUCE_(MAJOR_VERSION|MINOR_VERSION|BUILDNUMBER)" "$version_header"

# Written into the files that quote it, rather than left for someone to
# remember. They did not: `third_party` said 9.0.1 while the tree was on 9.0.2
# for a whole release cycle, and `third_party` is the attribution file that
# gets copied into all three published archives -- so the version travelling
# with the binary was the one that was wrong.
juce_version="$(sed -n 's/.*JUCE_MAJOR_VERSION[[:space:]]*\([0-9]*\).*/\1/p' "$version_header")"
juce_version+=".$(sed -n 's/.*JUCE_MINOR_VERSION[[:space:]]*\([0-9]*\).*/\1/p' "$version_header")"
juce_version+=".$(sed -n 's/.*JUCE_BUILDNUMBER[[:space:]]*\([0-9]*\).*/\1/p' "$version_header")"

if [[ ! "$juce_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "  could not parse a version out of $version_header (got '$juce_version')" >&2
  exit 1
fi

echo
echo "updating the files that quote the JUCE version to $juce_version:"

for quoting_file in "$ROOT/third_party" "$ROOT/README.md"; do
  if [[ -f "$quoting_file" ]] \
     && sed -i -E "s/JUCE 9\.[0-9]+\.[0-9]+/JUCE $juce_version/g" "$quoting_file"; then
    echo "  $(basename "$quoting_file"): $(grep -c "JUCE $juce_version" "$quoting_file") mention(s)"
  fi
done

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

# The directory is checked FIRST, because grep exits 2 for "no such directory"
# and 1 for "no match", and an `if grep ...; then ... else ... fi` cannot tell
# them apart -- both take the else branch. So a JUCE restructure that renames
# or moves juce_audio_devices would have turned the only automated standing
# check in BACKLOG.md into a permanent silent pass, reporting "as expected"
# about a directory it never read.
loopback_dir="$ROOT/lib/juce/modules/juce_audio_devices"

if [[ ! -d "$loopback_dir" ]]; then
  echo "  CHECK BROKEN: $loopback_dir does not exist." >&2
  echo "  JUCE has been restructured. This check cannot answer, and must not" >&2
  echo "  be read as 'no loopback'. Find the new path and update this script." >&2
  exit 1
fi

set +e
grep -ri loopback "$loopback_dir"
loopback_status=$?
set -e

case "$loopback_status" in
  0)
    echo "  ^^ loopback now appears in juce_audio_devices -- see the system-audio"
    echo "     capture entry in DECISIONS.md, which this check exists to revisit"
    ;;
  1)
    echo "  nothing (as expected) -- WASAPI loopback is still not exposed by JUCE"
    ;;
  *)
    echo "  CHECK BROKEN: grep failed with status $loopback_status" >&2
    exit 1
    ;;
esac

echo
echo "next: rm -rf build/release && cmake -S . --preset release && cmake --build build/release --config Release && ctest --test-dir build/release -C Release"
