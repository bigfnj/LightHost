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

# The split point, and the whole reason this take is usable. The analyser reads
# everything before QUIET_END as the quiet phase and everything from SPEAK_FROM
# as the speaking phase, and it has no way to detect either -- the phases are
# defined by what the operator was told to do.
#
# These numbers were only in analyse-voice-headroom.py, where the operator never
# sees them. The instruction was "quiet first, then speech" with no time in it,
# so a take that started speaking at four seconds produced plausible figures and
# no error: speech averaged into the noise floor and the headroom came out
# wrong in the safe-looking direction.
#
# Keep them in step with QUIET_END and SPEAK_FROM in analyse-voice-headroom.py.
quiet_end=6
speak_from=6.5
duration=18

device=${1:-Microphone (USB audio CODEC)}
out=${2:-voice_test.wav}

cat <<INSTRUCTIONS

  Recording ${duration}s in one take. Do NOT stop it early.

    0s to ${quiet_end}s    SILENCE. Do not speak, and do not touch the desk.
    after ${quiet_end}s    SPEAK normally, at the level you actually use,
                  and keep going until the recording stops.

  The analyser reads everything before ${quiet_end}s as the noise floor and
  everything from ${speak_from}s as speech, so the half second between them is
  margin for the first word. Speaking before ${quiet_end}s puts your voice in
  the noise floor, which makes the headroom figure wrong in the direction that
  looks fine.

INSTRUCTIONS

# The device name is a dshow string and machine-specific; it is the first thing
# to go stale on a different box. List them with:
#   ffmpeg -list_devices true -f dshow -i dummy
if ! ffmpeg -hide_banner -loglevel error -y -f dshow -audio_buffer_size 50 \
     -i audio="$device" -t "$duration" -ac 1 -ar 48000 \
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
