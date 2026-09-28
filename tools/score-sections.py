# -*- coding: utf-8 -*-
"""Score one render against a dry reference of the same take, section by section.

    tools/score-sections.py <dry.wav> <wet.wav>
    tools/score-sections.py dry.wav salvor.wav --label "Salvor @ 19 threads"
    tools/score-sections.py dry.wav out.wav --wet-offset-ms 10
    tools/score-sections.py dry.wav out.wav --section typing:12-24 --section speech:30-60

WHY SECTIONS, AND NOT A SINGLE WHOLE-FILE NUMBER

A denoiser changes two things at once and they pull in opposite directions: it
takes level out of the noise and it takes level out of the voice. One RMS delta
over the whole file adds those together and reports their sum, so 3 dB more
noise removed and 3 dB more voice damaged looks exactly like nothing happening.
That is not a number the denoiser question can be settled with -- DECISIONS.md's
own comparison table has "noise under speech" and "voice damage" as separate
columns for this reason, and the reversal trigger it records ("revisit if
artifacts are ever audible under load") is about the second column only.

Two sections is the minimum that separates them, and the two-phase take
tools/voice-headroom.sh records already has them: quiet by instruction first,
speech second. Those boundaries are read from analyse-voice-headroom.py rather
than repeated here, so a take recorded to one set of instructions cannot be
scored against another. --section overrides them for a take with more phases in
it, such as the 110-second one DECISIONS.md describes -- that take has no
recording script in this repo and no boundaries written down anywhere, so it
cannot be the default, but it can be scored by naming its sections here.

WHY THE HELPERS ARE RE-EXECUTED RATHER THAN IMPORTED

load, db, stats, QUIET_END and SPEAK_FROM all live in analyse-voice-headroom.py
and are wanted verbatim -- a scoring difference between two scripts that both
claim to measure RMS in dB is the kind of thing nobody finds. They cannot be
imported: the file name has a hyphen in it, so it is not a legal module name,
and the file runs its analysis at module scope with no __main__ guard, so
importing it would try to analyse sys.argv[1] and print its own report. Adding
a guard is a change to that file, which this one does not need to require. So
its top-level definitions -- imports, functions and SHOUTING_CASE constants --
are compiled and run on their own, and the statements that do work are left
behind. If a definition is renamed there, this fails loudly on the next run
rather than drifting.

WHAT IT DOES NOT DO

It prints deltas, not a verdict. One pair of files cannot tell a difference
between two plugins from a difference between two runs of the same plugin --
Salvor's output is not repeatable run to run, which is the whole reason
tools/denoiser-ab.ps1 repeats everything and takes medians. Judge from the
table that builds, not from one of these.
"""
import argparse
import ast
import pathlib
import sys
import wave

import numpy as np

HELPERS = pathlib.Path(__file__).resolve().with_name("analyse-voice-headroom.py")


def borrow(path):
    """Run only the definitions from `path` and hand back its namespace."""
    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        sys.exit("SCORE TOOL FAIL: cannot read %s: %s" % (path, exc))

    tree = ast.parse(source, filename=str(path))
    tree.body = [
        node for node in tree.body
        if isinstance(node, (ast.Import, ast.ImportFrom, ast.FunctionDef, ast.ClassDef))
        or (isinstance(node, ast.Assign)
            and all(isinstance(t, ast.Name) and t.id.isupper() for t in node.targets))
    ]

    namespace = {"__file__": str(path), "__name__": "analyse_voice_headroom"}
    exec(compile(tree, str(path), "exec"), namespace)
    return namespace


BORROWED = borrow(HELPERS)

try:
    load = BORROWED["load"]
    db = BORROWED["db"]
    stats = BORROWED["stats"]
    QUIET_END = BORROWED["QUIET_END"]
    SPEAK_FROM = BORROWED["SPEAK_FROM"]
except KeyError as missing:
    sys.exit("SCORE TOOL FAIL: %s no longer defines %s; this script borrows it"
             % (HELPERS.name, missing))


def load_any(name):
    """load(), plus the 24-bit case it does not cover.

    The render writes 24-bit WAVs -- OfflineRender.hpp asks for
    withBitsPerSample (24) -- and load()'s dtype table has entries for 2 and 4
    bytes only, so handing it a render output raises KeyError: 3. Everything
    about the 16- and 32-bit path stays where it is; only the width numpy has
    no dtype for is handled here.
    """
    with wave.open(name, "rb") as probe:
        width = probe.getsampwidth()

    if width != 3:
        return load(name)

    with wave.open(name, "rb") as w:
        rate, raw = w.getframerate(), w.readframes(w.getnframes())
        chans = w.getnchannels()

    # Little-endian three-byte samples widened to int32, then sign-extended:
    # bit 23 set means negative, and the value is that many below 2**24.
    b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
    v = np.where(v & 0x800000, v - 0x1000000, v)

    full = float(2 ** 23 - 1)   # the same "max of the type" load() uses
    clipped = int(np.count_nonzero(np.abs(v) >= 2 ** 23 - 2))
    a = v.astype(np.float64) / full

    if chans > 1:
        a = a.reshape(-1, chans).mean(axis=1)

    return rate, a, clipped


def parse_section(text):
    """NAME:START-END, in seconds. END may be the word end."""
    name, _, span = text.partition(":")
    start, _, end = span.partition("-")

    if not name or not start:
        raise argparse.ArgumentTypeError("want NAME:START-END, got %r" % text)

    try:
        lo = float(start)
        hi = None if end in ("", "end") else float(end)
    except ValueError:
        raise argparse.ArgumentTypeError("%r has a start or end that is not a number" % text)

    if hi is not None and hi <= lo:
        raise argparse.ArgumentTypeError("%r ends before it starts" % text)

    return (name, lo, hi)


parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument("dry", help="the reference render, or the take itself")
parser.add_argument("wet", help="the candidate render to score against it")
parser.add_argument("--label", default="", help="what this pair is, for the header line")
parser.add_argument("--wet-offset-ms", type=float, default=0.0,
                    help="trim this much off the front of the candidate, to undo the "
                         "chain's declared latency (the render prints it)")
parser.add_argument("--section", type=parse_section, action="append", default=[],
                    metavar="NAME:START-END",
                    help="repeatable; replaces the default quiet/speech split")
args = parser.parse_args()

try:
    dry_rate, dry, dry_clipped = load_any(args.dry)
    wet_rate, wet, wet_clipped = load_any(args.wet)
except (OSError, wave.Error, EOFError, KeyError) as exc:
    sys.exit("SCORE TOOL FAIL: %s: %s" % (type(exc).__name__, exc))

if dry_rate != wet_rate:
    sys.exit("SCORE TOOL FAIL: %d Hz against %d Hz -- these are not the same take"
             % (dry_rate, wet_rate))

# The chain delays audio by its declared latency and the render does not
# compensate, so the candidate sits later in time than the reference by that
# much. Salvor declares 10 ms and Alt Denoiser 40 ms; leaving that in would put
# a systematic difference between the two candidates into every section
# boundary, which is precisely the comparison being made.
offset = int(round(args.wet_offset_ms * wet_rate / 1000.0))

if offset > 0:
    if offset >= len(wet):
        sys.exit("SCORE TOOL FAIL: --wet-offset-ms %.1f is longer than the candidate"
                 % args.wet_offset_ms)
    wet = wet[offset:]

# The render appends the declared latency plus one block so a delayed plugin is
# not cut off, so the candidate is longer than the reference. Compare the span
# both actually have.
usable = min(len(dry), len(wet))

if usable < wet_rate // 2:
    sys.exit("SCORE TOOL FAIL: only %.2f s is common to both files; nothing to score"
             % (usable / float(wet_rate)))

dry = dry[:usable]
wet = wet[:usable]
seconds = usable / float(wet_rate)

sections = args.section or [("quiet", 0.0, QUIET_END),
                            ("speech", SPEAK_FROM, None),
                            ("whole", 0.0, None)]

print("score-sections: %s" % (args.label or "%s vs %s"
                              % (pathlib.Path(args.dry).name, pathlib.Path(args.wet).name)))
print("  dry           : %s (%d at the ceiling)" % (args.dry, dry_clipped))
print("  wet           : %s (%d at the ceiling, front trimmed %.1f ms)"
      % (args.wet, wet_clipped, args.wet_offset_ms))
print("  compared over : %.2f s at %d Hz" % (seconds, wet_rate))
print()

scored = 0

for name, lo, hi in sections:
    end = seconds if hi is None else min(hi, seconds)

    if lo >= seconds:
        print("SKIP %-8s starts at %.1f s, past the %.2f s both files have" % (name, lo, seconds))
        continue

    if end - lo < 0.2:
        print("SKIP %-8s is only %.2f s long here; too short to mean anything"
              % (name, max(0.0, end - lo)))
        continue

    a, b = int(lo * wet_rate), int(end * wet_rate)
    d_rms, d_peak = stats(dry[a:b], wet_rate, "dry  %s %.1f-%.1f s" % (name, lo, end))
    w_rms, w_peak = stats(wet[a:b], wet_rate, "wet  %s %.1f-%.1f s" % (name, lo, end))

    # Negative is the denoiser taking level out. In the quiet section that is
    # the thing it is for; in the speech section it is the damage it does.
    print("DELTA %-8s dRMS %+7.2f   dPeak %+7.2f" % (name, w_rms - d_rms, w_peak - d_peak))
    print()
    scored += 1

if scored == 0:
    sys.exit("SCORE TOOL FAIL: no section fitted inside %.2f s of audio" % seconds)

print("Deltas only. Whether one of these is better than another is a question")
print("about repeats and spread, which one pair of files cannot answer -- see")
print("tools/denoiser-ab.ps1.")
