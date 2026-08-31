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
