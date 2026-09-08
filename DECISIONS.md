# Design decisions

Things that were asked for, assessed, and deliberately not built — and what would
have to change for the answer to be different.

This is not [BACKLOG.md](BACKLOG.md). That file holds known bugs and work still
intended. This one holds work that was considered and declined, so that the
reasoning survives the conversation it happened in and the question does not have
to be re-answered from scratch every six months.

Each entry records what was actually asked, what it would require, why the answer
is no, and what would change it.

---

## Denoiser selection — Salvor kept, three alternatives declined 2026-09-08

### What was asked

Which real-time speech denoiser should sit in the chain, given five candidates
installed on the machine: Salvor (scintillator), Alt Denoiser (Altinus), Elgato
Noise Removal, Elgato Voice Focus, and Cockos ReaFIR.

### How it was measured

A purpose-built 110-second take with the sections separated in time — silence,
typing alone, claps and snaps, speech in a quiet room, speech with typing
underneath — so that noise reduction and voice damage could be attributed
separately rather than inferred from a mixture. Each candidate rendered twice
through `--render --chain`, paced to real time, scored per section.

The method matters more than the numbers, because the first version of it was
wrong in a way that produced confident nonsense. See "Measuring a plugin that
does its work on a worker thread" below.

### The result

| | Noise under speech | Voice damage | Latency | Repeatable |
|---|---|---|---|---|
| **Salvor** (chosen) | −5.08 dB | −1.73 dB | **10 ms** | no |
| Alt Denoiser | −5.09 dB | −1.73 dB | 40 ms | yes |
| Elgato Noise Removal | −0.52 dB | −0.11 dB | 0 ms | yes |
| Elgato Voice Focus | 0.00 dB | 0.00 dB | 0 ms | — |

**Salvor and Alt Denoiser are the same model** — both wrap DeepFilterNet3 — and
score identically to two decimals on typing removal (−25.4 vs −25.5 dB), voice
damage, and noise under speech. The decision is therefore latency against
robustness, not audio quality.

**Alt Denoiser declined on latency.** 40 ms against Salvor's 10 ms. Its
advantages are real but not audible: a silence floor 32 dB deeper (−118 dB vs
−86 dB, both far below hearing) and bit-identical repeatability. Salvor runs
inference on a worker thread and its output varies run to run; under a soak test
with 19 of 20 cores pegged it lost 2.4 dB of silence-floor depth and nothing
else. Thirty milliseconds of conversational latency is worth more than 2.4 dB of
an inaudible floor. **Revisit if artifacts are ever audible under load** — Alt
Denoiser is provably immune, being synchronous.

**Elgato Noise Removal declined as a complement.** It is a voice-activity gate
(`VAD Threshold`, `VAD Release`), not a denoiser: it drove the typing-only
section to digital zero but removed only 0.52 dB from under speech. Stacking it
after Salvor was considered and rejected — Salvor already puts pauses at −86 dB,
so the gate would buy an inaudible improvement while risking clipped word onsets,
and Teams and Zoom run their own VAD anyway.

**Elgato Voice Focus is unusable.** Inert with `Bypass` forced off and
`Enhancement` at maximum: every metric 0.00. It is licence-gated internally and
the activation has never completed. Not a settings problem; nothing to tune.

**ReaFIR not tested.** Its noise profile is state, not a parameter — it must be
trained through the GUI and frozen — so a default-state render would report zero
reduction and mean nothing. A 2016 subtractive-FFT processor whose profile goes
stale whenever the room changes was not worth the setup against a model that
needs no training.

### Settings that came out of it

- Salvor `Attenuation Limit = 0`. The control caps reduction at that many dB and
  **0 means no cap**: measured −6.02 dB at −6, −11.97 at −12, −17.56 at −18, and
  −24.8 uncapped. 0 is best on both axes at once, giving the most reduction *and*
  the least voice damage.
- Microphone input gain **+22 dB** (93.2% on the Windows slider). This matters
  more than any plugin setting: the same audio at +20 dB gave −11.2 dB of silence
  reduction and at +22 dB gave −28.5 dB. Below roughly −34 dB RMS of speech the
  model barely engages. Verified that analog gain scales captured noise like
  digital gain, so the sweep's digitally-boosted proxy was sound.

### Measuring a plugin that does its work on a worker thread

Recorded here because the wrong answer was published twice before the method was
fixed, and the failure is silent.

An unpaced offline render runs about thirty times real time. A plugin doing
neural inference on a background thread cannot keep up, falls back to passing the
dry signal through, and the render reports success. Salvor measured **0.55 dB**
of noise reduction that way, and on a quieter take appeared to *raise* the noise
floor by 8.5 dB — which produced a confident recommendation to remove it from the
chain. Paced, it removes 24.8 dB.

The tell that something was wrong was not in the numbers. It was that 0.55 dB
against obvious keyboard clatter is not a plausible result for any denoiser, so
the method deserved suspicion before the plugin did. Renders are now paced by
default for exactly this reason.

---

## Acoustic echo cancellation — declined 2026-08-28

### What was asked

"Is there a way to block PC output from showing up in the mic input?" — that is,
stop audio coming out of the speakers from being picked up by the microphone and
processed as if it were the user's voice.

This is worth separating from the question it usually arrives attached to. It is
**not** WASAPI loopback capture, which is about *processing* system audio. It is
**acoustic echo cancellation**: removing the speakers from the microphone signal.

The two are connected, and that connection is the whole reason for the answer.
AEC works by subtracting a **reference** — what is being played — from the
microphone signal. Obtaining that reference is exactly what loopback is for. So
loopback is not the goal here; it is the prerequisite.

### Why the answer is no

Four constraints, in ascending order of how hard they are to remove.

**1. The graph has no sidechain.** `GraphTopology::Layout` carries exactly one
`inputNodeId`. `NodeFacts` is `{nodeId, lane, numInputChannels,
numOutputChannels}` — there is no notion of a second source, and `appendEdge`
wires one source to one destination. An AEC node needs two inputs, microphone and
reference, and `buildConnections` cannot express that. This is the most tractable
of the four: a new concept in a pure function, plus its tests.

**2. Only one audio device is open.** `IconMenu` holds a single
`juce::AudioDeviceManager`, initialised as `initialise (2, 2, …)`. AEC needs the
microphone and the reference arriving together. A second device would have to be
opened independently and handed across on a lock-free ring buffer. That is
strictly larger than the `AudioEngine` extraction in BACKLOG.md, which already
carries a warning about being the riskiest refactor in the project.

**3. JUCE exposes no loopback.** `WASAPIDeviceMode` is `shared`, `exclusive` and
`sharedLowLatency`; the string `loopback` appears nowhere in
`juce_audio_devices`. Windows itself has supported loopback since Vista
(`AUDCLNT_STREAMFLAGS_LOOPBACK` on a render endpoint) and added a per-process API
in Windows 10 2004, so this is a framework gap rather than a platform one — see
the re-check note under Deferred in BACKLOG.md. Without it, the reference has to
come from a hand-rolled WASAPI backend or from a virtual cable carrying a copy of
the output.

**4. Two clocks, and this is the one that decides it.** The microphone and the
output are separate endpoints on separate crystals — a USB interface and a dock,
or a virtual cable. They drift, on the order of 10–100 ppm, which is a sample
every few seconds. An adaptive filter whose reference slides against the echo it
is trying to cancel does not merely underperform: it **diverges, and sounds worse
than doing nothing**. Every serious implementation (WebRTC, Speex) carries drift
estimation and resampling for exactly this reason.

On top of the four, the DSP itself is specialist work: an adaptive filter,
double-talk detection so the user is not cancelled when they speak over the
speakers, and nonlinear residual suppression because speakers distort and a linear
filter cannot cancel what a nonlinearity added. The realistic route is to vendor
WebRTC's `audio_processing` module rather than write it — and constraints 1 to 3
are still prerequisites to feeding it.

Taken together this is a different application, not a feature.

### What solves the problem instead

In order of how well they work.

1. **Headphones.** No code, completely effective, because the echo path stops
   existing. Every other option on this list is partial.
2. **Send playback where the microphone cannot hear it** — a virtual cable, with
   monitoring on headphones. Same outcome as the above using equipment a user
   running this host probably already has.
3. **Let the conferencing application do it.** This is the substantive point:
   Teams, Zoom, Discord and the rest *can* cancel echo correctly and Light Host
   structurally cannot, because they know precisely what they just played and
   Light Host has no idea. It is why they all ship echo cancellation. The sensible
   division of labour is that Light Host shapes tone and the application cancels
   echo — which also means their noise suppression is worth leaving **on** when
   speaker bleed is the problem, even though it is worth turning off when it is
   not.
4. **The capture driver's own AEC**, where one exists — Sound Control Panel, the
   microphone's Enhancements tab. Driver-dependent and absent on most generic USB
   audio interfaces, but free to check.

A gate or expander does **not** solve it. Speech from the speakers and speech from
the user are both speech at similar levels, and nothing keyed on level alone can
tell them apart.

### What would change the answer

- JUCE growing a loopback device mode, which removes constraint 3 and makes the
  reference obtainable without a virtual cable or a forked backend.
- A decision to support two simultaneous input devices for some other reason,
  which removes constraint 2 and leaves this a much smaller piece of work.

Constraint 4 does not go away in either case. Any implementation has to handle
clock drift, and an implementation that does not is worse than no implementation
at all.
