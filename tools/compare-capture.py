# -*- coding: utf-8 -*-
"""Compare two capture endpoints, to tell "chain broken" from "chain bypassed".

The host can report a perfectly wired graph and still be irrelevant, because
Windows keeps a separate Communications default and calling apps follow it. So
the question "is the chain working?" is answered by looking at what comes OUT of
the virtual cable, not by reading the host's log.

Run both captures with nobody speaking. The raw microphone should show room
tone; the cable should be much quieter, because the denoiser and the gate are
doing their job. If the two look the same, the chain really is not processing.
"""
import sys
import wave

import numpy as np


def load(name):
    with wave.open(name, "rb") as w:
        rate, raw = w.getframerate(), w.readframes(w.getnframes())
        width, chans = w.getsampwidth(), w.getnchannels()
    dt = {2: np.int16, 4: np.int32}[width]
    a = np.frombuffer(raw, dtype=dt).astype(np.float64) / float(np.iinfo(dt).max)
    if chans > 1:
        a = a.reshape(-1, chans).mean(axis=1)
    return rate, a


def db(x):
    return 20.0 * np.log10(max(float(x), 1e-12))


def bands(sig, rate):
    n = 4096
    acc = np.zeros(n // 2 + 1)
    win = np.hanning(n)
    hops = 0
    for i in range(0, max(1, len(sig) - n), n // 2):
        if i + n <= len(sig):
            acc += np.abs(np.fft.rfft(sig[i:i + n] * win)) ** 2
            hops += 1
    acc /= max(hops, 1)
    f = np.fft.rfftfreq(n, 1.0 / rate)
    return {(lo, hi): db(np.sqrt(acc[(f >= lo) & (f < hi)].sum()))
            for lo, hi in ((50, 200), (200, 500), (500, 1000),
                           (1000, 3000), (3000, 8000), (8000, 16000))}


rate, raw = load(sys.argv[1])
_, chain = load(sys.argv[2])

print("%-28s %10s %10s" % ("", "RAW mic", "CABLE out"))
print("%-28s %10.2f %10.2f" % ("RMS (dBFS)", db(np.sqrt(np.mean(raw ** 2))),
                               db(np.sqrt(np.mean(chain ** 2)))))
print("%-28s %10.2f %10.2f" % ("peak (dBFS)", db(np.max(np.abs(raw))),
                               db(np.max(np.abs(chain)))))
print("%-28s %10d %10d" % ("non-zero samples", int(np.count_nonzero(raw)),
                           int(np.count_nonzero(chain))))
print()
br, bc = bands(raw, rate), bands(chain, rate)
print("band            RAW       CABLE     difference")
for k in sorted(br):
    print("%5d-%-6d %8.2f  %8.2f   %+8.2f dB" % (k[0], k[1], br[k], bc[k], bc[k] - br[k]))

delta = db(np.sqrt(np.mean(chain ** 2))) - db(np.sqrt(np.mean(raw ** 2)))
print()
print("cable is %.2f dB relative to the raw mic" % delta)
if delta < -12.0:
    print("VERDICT: the chain IS processing -- the cable is far quieter than the raw")
    print("         mic with nobody speaking, which is the denoiser and gate working.")
elif delta > -3.0:
    print("VERDICT: the cable looks like the raw mic. The chain is NOT processing.")
else:
    print("VERDICT: inconclusive at this level difference; talk and re-run.")
