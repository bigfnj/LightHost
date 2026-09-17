#!/bin/bash
# Two-phase capture: quiet first, then speech, in one take.
#
# Measured separately because a single figure cannot tell them apart. A capture
# taken while the speakers are bleeding into the microphone shows the spectrum
# and dynamics of speech -- because it IS speech -- so "is this the user talking"
# cannot be answered from the signal. Splitting the take by instruction is the
# only reliable way to know which is which.
#
# set -euo pipefail, and the checks below, because this used to print
# "CAPTURE DONE" unconditionally: ffmpeg's exit status was never read, so a
# missing device, a device name that had changed, or a disk that was full all
# reported the same success as a good take, and the analysis that followed ran
# on whatever voice_test.wav happened to be lying around from last time.
set -euo pipefail

device=${1:-Microphone (USB audio CODEC)}
out=${2:-voice_test.wav}

# The device name is a dshow string and machine-specific; it is the first thing
# to go stale on a different box. List them with:
#   ffmpeg -list_devices true -f dshow -i dummy
if ! ffmpeg -hide_banner -loglevel error -y -f dshow -audio_buffer_size 50 \
     -i audio="$device" -t 18 -ac 1 -ar 48000 \
     -c:a pcm_s16le "$out"; then
  echo "CAPTURE FAILED: ffmpeg could not record from \"$device\"." >&2
  echo "List the devices with: ffmpeg -list_devices true -f dshow -i dummy" >&2
  exit 1
fi

# Non-empty, not merely present: ffmpeg can exit 0 having written a header and
# no samples if the device opens and delivers nothing.
if [[ ! -s "$out" ]]; then
  echo "CAPTURE FAILED: $out is empty, so the device delivered no audio." >&2
  exit 1
fi

echo "CAPTURE DONE: $out ($(wc -c < "$out") bytes)"
