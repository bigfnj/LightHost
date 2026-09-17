# -*- coding: utf-8 -*-
"""Split a two-phase take into its quiet and speaking halves and report both.

The point of the split is that a single number cannot separate the user's voice
from speech arriving acoustically out of the speakers. Both look like speech,
because both are. So the quiet phase is labelled by instruction, not detection,
and every figure below is anchored to that.
"""
import sys
import wave

import numpy as np

# These define the phases; nothing here detects them. Keep them in step with
# quiet_end and speak_from in voice-headroom.sh, which is where the operator is
# told what to do -- until 5.4.0 that script said only "quiet first, then
# speech" and named no time at all, so a take that began speaking early gave
# plausible figures and no error.
QUIET_END = 6.0     # seconds; the performer was asked to stay silent until here
SPEAK_FROM = 6.5    # margin, so the first word is not counted into the quiet half


def load(name):
    with wave.open(name, "rb") as w:
        rate, raw = w.getframerate(), w.readframes(w.getnframes())
        width, chans = w.getsampwidth(), w.getnchannels()
    dt = {2: np.int16, 4: np.int32}[width]
    v = np.frombuffer(raw, dtype=dt)
    full = float(np.iinfo(dt).max)
    clipped = int(np.count_nonzero(np.abs(v.astype(np.int64)) >= np.iinfo(dt).max - 1))
    a = v.astype(np.float64) / full
    if chans > 1:
        a = a.reshape(-1, chans).mean(axis=1)
    return rate, a, clipped


def db(x):
    return 20.0 * np.log10(max(float(x), 1e-12))


def stats(seg, rate, label):
    f = rate // 10
    n = len(seg) // f
    lv = np.array([db(np.sqrt(np.mean(seg[i * f:(i + 1) * f] ** 2))) for i in range(n)])
    print("%-22s RMS %7.2f   peak %7.2f   p90 %7.2f   p10 %7.2f"
          % (label, db(np.sqrt(np.mean(seg ** 2))), db(np.max(np.abs(seg))),
             np.percentile(lv, 90), np.percentile(lv, 10)))
    return db(np.sqrt(np.mean(seg ** 2))), db(np.max(np.abs(seg)))


rate, a, clipped = load(sys.argv[1])
quiet = a[:int(QUIET_END * rate)]
speak = a[int(SPEAK_FROM * rate):]

print("take length %.1f s, %d samples at the ceiling in the whole file" % (len(a) / rate, clipped))
print()
q_rms, q_pk = stats(quiet, rate, "QUIET (0-6 s)")
s_rms, s_pk = stats(speak, rate, "SPEAKING (6.5 s+)")
print()
print("per-second level:", " ".join("%.0f" % db(np.sqrt(np.mean(a[i * rate:(i + 1) * rate] ** 2)))
                                    for i in range(len(a) // rate)))
print()
print("voice above bleed/floor : %+7.2f dB" % (s_rms - q_rms))
print("headroom on peaks       : %7.2f dB" % max(0.0, -s_pk))
print()
if s_rms - q_rms < 10.0:
    print("The quiet phase is within 10 dB of the speaking phase. Whatever is")
    print("coming out of the speakers is nearly as loud at the microphone as you")
    print("are -- no denoiser separates that, because both are speech.")
elif s_rms - q_rms < 20.0:
    print("Usable but compromised: the bleed sits close enough to your voice that")
    print("the far end will hear itself through the chain.")
else:
    print("Healthy separation between your voice and the room.")
