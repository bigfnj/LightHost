#!/bin/bash
# Two-phase capture: quiet first, then speech, in one take.
#
# Measured separately because a single figure cannot tell them apart. A capture
# taken while the speakers are bleeding into the microphone shows the spectrum
# and dynamics of speech -- because it IS speech -- so "is this the user talking"
# cannot be answered from the signal. Splitting the take by instruction is the
# only reliable way to know which is which.
ffmpeg -hide_banner -loglevel error -y -f dshow -audio_buffer_size 50 \
  -i audio="Microphone (USB audio CODEC)" -t 18 -ac 1 -ar 48000 \
  -c:a pcm_s16le voice_test.wav
echo "CAPTURE DONE"
