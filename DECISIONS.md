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

## Always-on metering probes — declined 2026-09-16

### What was asked

Whether the per-plugin metering probes added in 5.1.0 should live in the audio
graph permanently, so that a clip on plugin three is caught even when the signal
view is closed, or be created only while that column is open.

### Why the answer is "only while open"

The argument for always-on was that the incident which motivated the whole
feature happened while nobody was watching, so a diagnostic that only works when
observed is no diagnostic at all. That argument is sound, and it is why the
**device** meters are always active.

It does not extend to the probes, because the incident was **input** clipping.
The input meter catches that, and its cost is a SIMD `findMinAndMax` over roughly
96k samples per second -- unmeasurable. Once the always-on half already covers
the failure that actually occurred, per-plugin probes have nothing left to earn,
and N extra nodes scanning every block forever is cost with no return.

The measured cost of the expensive half is what made this decidable rather than a
matter of taste. Peak is SIMD and effectively free; RMS is a per-sample sum of
squares, and it is the only part that needed gating. So metering is split by
what it is for: peak always, RMS only while a visible meter holds a watcher.

### What would change the answer

A plugin that fails intermittently mid-chain, where the failure is not visible at
the input or the output. Nothing observed so far fits: every problem this project
has had was visible at one end or the other. If one turns up, always-on probes at
peak-only cost would be roughly the same price as the device meters, and the
gating machinery to do it already exists.

---

## `AudioEngine` extraction — declined 2026-09-16

### What was asked

Split `deviceManager`, `player` and `graph` out of `IconMenu` into a separate
`AudioEngine` class. `Source/IconMenu.cpp` is 2,200 lines, and the split would
both shorten it and create a seam a unit test could reach without constructing a
device manager and a tray icon. It is the remainder of the 5.0.0 plan's Phase 5,
carried in [BACKLOG.md](BACKLOG.md) until this decision cleared it.

### Why the answer is no

**The invariant being moved is four ordering constraints that pull in opposite
directions, not one.** They all live in `~IconMenu` in `Source/IconMenu.cpp`,
plus the member declaration order in `Source/IconMenu.hpp`.

**1. The real-time thread has to be severed first.** `deviceManager` is declared
before `graph`, `player` and `deviceTap`, so it is destroyed after all three.
Until `deviceManager.removeAudioCallback (&deviceTap)` (the 3.2.0 changelog entry says `&player`, correct for the time — `DeviceTap` was inserted between the device and the player in 5.1.0) returns, the device's
callback thread is still running `DeviceTap` → `AudioProcessorPlayer` → `graph`
on objects that are about to be destroyed. `removeAudioCallback` blocks on JUCE's
own lock until any in-flight callback has finished, which is what makes every
statement after it safe. This was a real crash, fixed in v3.2.0 and written up in
[CHANGELOG.md](CHANGELOG.md): on a tray-menu quit the window was narrow enough to
survive sometimes, and on Windows restart or shutdown it was fatal every time,
because the OS disrupts audio services in parallel with the application's own
teardown and that widens the race.

**2. `player.setProcessor (nullptr)`.** This drops the player's pointer to
`graph` at a known statement in the destructor body, rather than leaving it to
`~AudioProcessorPlayer` — which does exactly the same thing, at a point decided
by member declaration order rather than by anything visible at the call site.

**3. `removeChangeListener` and `cancelPendingUpdate()` sever the message
thread**, which is a separate race from the first one and is easy to read as part
of it. A hosted plugin that re-declares its latency arrives through
`audioProcessorChanged` on any thread and is deferred to an `AsyncUpdater`; an
update already queued would otherwise rewire the graph during teardown.

**4. `savePluginStates()` requires the graph to still be alive.** It looks each
plugin up with `graph.getNodeForId` and asks the processor for
`getStateInformation`, which only an existing plugin can answer. That is why it
is the last statement of the destructor body rather than the first.

**Constraint 4 is what makes the split actively dangerous.** The plan the
backlog carried was to move the detach sequence "wholesale into `AudioEngine`'s
destructor". If `AudioEngine` is a member of `IconMenu`, the body of `~IconMenu`
runs *before* `~AudioEngine` — so `savePluginStates()` would be calling
`getStateInformation` on plugins that are still being driven by a live audio
callback, concurrently with their own `processBlock`. Following the plan as
written introduces a new data race, in the same destructor whose ordering the
plan exists to protect, and no compiler warns about any of it. The only correct
shape is an explicit, idempotent `engine.detach()` as the first statement of
`~IconMenu`, with the destructor of `AudioEngine` calling it again in case
nobody did.

**The sanitiser gate offered to justify that risk cannot be built.** The backlog
proposed running the smoke test under a sanitiser as part of the gate. No CI
runner has an audio device, so `getCurrentAudioDevice()` returns null and no
callback thread ever starts. `Source/SelfTest.hpp` says so in its own header
comment: "This proves the ordering code runs without faulting; it does not
reproduce the race." An AddressSanitizer job would therefore be green whether or
not the invariant holds. It would gate nothing while looking like it gated
something, which is worse than having no gate, because the next person reads a
green check and believes it.

**ASan is impractical here for a second, independent reason.** This host loads
third-party VST2 and VST3 plugin DLLs into its own process. ASan's interceptors
are process-wide, so a plugin with a custom allocator — which is most of them —
produces reports about code this project did not write and cannot fix, and
writing suppressions for closed-source binaries is guesswork that has to be
redone every time a vendor ships an update.

**It could not gate the configuration that ships anyway.** Release on MSVC uses
`/GL` and `/LTCG` (see `CMakeLists.txt`), which is incompatible with
`/fsanitize=address`. An instrumented job would be building something other than
the binary that goes out.

**There is no user-visible payoff.** Every other backlog item cleared in 5.2.0
either changed what the application does or closed a verification gap. This one changes how long a file is, which is
a readability preference, paid for with the worst failure mode in the project.

Two of the three seams the refactor would create have already been collected
without it. `PreferencesWindow` takes `juce::AudioDeviceManager&` by reference
rather than reaching into `IconMenu` for it, and the graph's wiring *decision* is
already a pure function in `Source/GraphTopology.hpp` — `buildConnections` and
`applyConnections` in the `lighthost::topology` namespace — with its own tests in
`Tests/GraphTopologyTests.cpp`. What is left unseamed is ownership of the three
objects, and ownership is the part that carries the shutdown invariant.

### What would change the answer

A way to reproduce the shutdown race automatically: a virtual or loopback audio
device on a CI runner, opened by the self-test, driving a real callback thread
through a real teardown. With that in place the invariant becomes testable, the
sanitiser gate becomes worth building, and the argument against the split is only
that it is not needed. Failing that, the extraction becoming a prerequisite for a
feature someone has asked for, rather than for tidiness.

---

## Allocated slot ids and duplicate plugins in one chain — declined 2026-09-16

### What was asked

Two requests that turn out to be one. Let the same plugin appear twice in a
single chain, and key its settings by an allocated slot id instead of by a hash
of the plugin's identity, so that relocating a plugin file does not orphan its
order, lane, bypass and saved state.

### Why the answer is no

**Both are blocked on the same thing: the chain no longer being a
`juce::KnownPluginList`.** The backlog carried them as two
separate deferred items, which understated how tightly they are coupled.

**The key scheme does collide, and the container is currently hiding it.**
`chain::Store::identityOf` in `Source/PluginChainStore.hpp` hashes
`fileOrIdentifier`, `pluginFormatName`, `uniqueId` and `deprecatedUid`. Two
instances of one plugin agree on every one of those, so they would share one set
of settings — one order value, one lane, one bypass, one state blob. It is
unreachable today only because `KnownPluginList::addType` refuses the second
`addType` outright, comparing with `PluginDescription::isDuplicateOf`, which ties
`fileOrIdentifier`, `deprecatedUid` and `uniqueId` — the same fields the key is
built from, less the format. So the container is masking a real defect in the key
scheme rather than the key scheme being sound. Removing the container unmasks it.
Slot ids are a prerequisite for duplicates, not a follow-up, and the two have to
land in the same change.

**`KnownPluginList` is load-bearing for three things beyond uniqueness.** It is
the entire on-disk format of chain membership: `pluginListActive` is whatever
`createXml` produced, read back by `recreateFromXml`. It broadcasts its own
changes, and `IconMenu::changeListenerCallback` turns that broadcast into the
settings write, so three mutation sites in `Source/IconMenu.cpp` — the tray
delete, and the add and remove halves of `applyPluginChain` — persist the chain
without asking; each carries a comment saying so, and each would have to grow an
explicit save. And four places in the tree are coupled to that XML shape, in
three different ways: `Source/IconMenu.cpp` decodes it into the live
`activePluginList`, `Source/OfflineRender.hpp` constructs throwaway
`KnownPluginList` objects purely to parse it (one for the chain, one for the
scanned list), `Source/SelfTest.hpp` constructs one purely to *write* a fixture
in the 4.0.3 format, and `Tests/PluginChainStoreTests.cpp` hard-codes the
`KNOWNPLUGINS` element name as a string literal. A replacement container has to
satisfy all four.

**The settings format is versioned but the migration hook cannot carry this.**
`chain::Store` has `kFormatVersion` and stores it under `chainSettingsVersion`,
which looks like the mechanism for exactly this change. It is not, for two
reasons. `pluginListActive` sits outside the store entirely — `purgeLegacyKeys`
is explicitly written so the prefix cannot match it — so the store's migration
never sees the part of the format that would be changing. And `migrateIfNeeded`
is a single monolithic step guarded by `if (formatVersion() >= kFormatVersion)`,
so bumping the version re-runs the 4.0.3 key migration and `purgeLegacyKeys` on
every install that has already been migrated. That is harmless today, because
there is nothing left for it to find, but it means the version number cannot be
used for its purpose until it is split into per-step migrations.

**Nothing has asked for either.** The settings-loss case the slot ids would fix
requires a user to move their plugin folder, which nobody has reported. There is
no UI blocker worth counting against it — the active chain is a hand-written
component, not a `PluginListComponent`, so it has no opinion about uniqueness —
which means the whole cost sits in the settings format and the persistence
broadcast, and none of it buys anything anyone currently wants.

### What would change the answer

A report of chain settings lost after moving a plugin folder, which is the defect
the allocated ids exist to fix, or a genuine need for two instances of one plugin
in one chain. Either one makes the container change worth doing, and it has to be
done once for both.

---

## Endpoint-id audio device selection — declined for now 2026-09-16

### What was asked

Store a stable Windows audio endpoint id alongside the device name, so that
renaming a device — or a driver renaming it — does not silently select a
different device on the next launch.

### Why the answer is no

**JUCE identifies audio devices by display name only.** `AudioDeviceSetup`
carries `inputDeviceName` and `outputDeviceName` and nothing else that identifies
a device. The persisted state is a `DEVICESETUP` element with
`audioInputDeviceName` and `audioOutputDeviceName` attributes and no id, written
and read in `juce_AudioDeviceManager.cpp` under
`lib/juce/modules/juce_audio_devices/audio_io`. A name is the only thing there is
to store.

**No public JUCE API exposes or accepts an audio endpoint id.** JUCE does
precisely the id-then-name fallback being asked for, but only for MIDI:
`openLastRequestedMidiDevices` matches on `MidiDeviceInfo::identifier` first and
falls back to the name. There is no audio equivalent. The WASAPI backend does
hold the endpoint ids — `IMMDevice::GetId`, `PKEY_Device_FriendlyName` and
`appendNumbersToDuplicates` are all in `juce_WASAPI_windows.cpp` — but they live
in a structure local to that file and are not reachable from this project without
patching the vendored tree, which `tools/update-juce.sh` would discard on the
next bump.

**Doing it outside JUCE means duplicating that backend.** Windows COM
(`IMMDeviceEnumerator`, `IMMDevice::GetId`, `PKEY_Device_FriendlyName`), a new
settings key with its own migration, and a reimplementation of JUCE's
`appendNumbersToDuplicates` suffixing so an endpoint id maps back to the exact
string the device list shows for two endpoints that share a friendly name. It
would be WASAPI-only, because an ASIO device name is a driver name and has no
endpoint behind it, and it would be untestable on CI, which has no real
endpoints. That is a Windows-specific reimplementation of vendored code, to fix a
problem that has been reported zero times.

**5.2.0 fixes the harm instead of the mechanism.** The damage was never the
fallback itself — falling back to the default device is the right behaviour when
the requested one is gone. The damage was that it happened in silence, so audio
turned up somewhere unexpected with nothing on screen to explain it.
`Source/DevicePolicy.hpp` compares what was requested against the names actually
open, and reports the difference by name: which role changed, what was asked
for, and what is being used instead. A user who can see that their interface is
missing can re-select it; a user who cannot see it has no way to know there is
anything to do.

**Updated 2026-09-17.** 5.2.0 read the request out of the stored `DEVICESETUP`,
on the grounds that JUCE's fallback path does not rewrite it. That is true of
the fallback and false of everything else: `autoMatchSampleRate`, Preferences
Apply and `setCurrentAudioDeviceType` all pass `treatAsChosenDevice = true`,
which calls `updateXml()` and adopts the fallback device as the stored choice.
After any of them the request was gone and the substitution became permanently
undetectable.

Light Host now records the names the user picked under its own key,
`lighthost::keys::requestedDevices`, written only from the Preferences combo
boxes and only when one of them actually changed. `DEVICESETUP` remains the
fallback for a first run and for settings written by 5.2.0. None of this
changes the answer below about endpoint ids: the comparison is still by name,
because a name is still the only thing JUCE gives us.

### What would change the answer

JUCE exposing a stable audio device identifier, which turns this into the same
few lines the MIDI path already has. Or the reported fallback proving
insufficient in practice — someone seeing the message, and still ending up on the
wrong device without understanding why.

---

## Dry/wet, a spectrum view, out-of-process hosting — declined 2026-09-16

### What was asked

Three separate requests, each small enough that the reasoning fits in a
paragraph, recorded together so none of them has to be re-derived.

### Why the answer is no

**Dry/wet per lane.** Nothing has asked for it. It also is not the UI addition it
looks like: `buildConnections` in `Source/GraphTopology.hpp` groups only nodes
that can pass audio into lanes, so a lane with no plugin in it does not appear in
the graph at all and its trim node is left unwired. A blend against the
unprocessed input therefore needs new wiring — a path from the input node to the
lane's summing point that the function never builds today — rather than a control
hung off something that already exists. And the plugins in the chain that would
want it carry their own mix controls, which is the normal place for a wet/dry
blend to live.

**A spectrum or waveform signal view.** The signal view added in 5.1.0 answers
the question that was actually being asked, which was *where in the chain the
level changes* — the delta column exists because working out that one plugin was
adding 7 dB took a paced offline render and an analysis script when the number
was one hop away in the graph. A spectrum answers a different question, and costs
an FFT per probe on the audio thread. That is a different cost class from peak
metering, and the cost is exactly what was scoped in "Always-on metering probes"
above: peak is a SIMD `findMinAndMax` and effectively free, RMS is a per-sample
sum of squares and had to be gated on a visible watcher. An FFT per probe per
block sits above both.

**Out-of-process plugin hosting.** It is the one item here with a real benefit:
plugins run in-process, so a plugin that crashes takes the host with it, and a
child process would make that survivable. It is also a different application — IPC carrying audio under a real-time
deadline, embedding a plugin editor across a process boundary, and a new failure
mode to design for when the child dies and the chain has to carry on without it.
Real sandboxing is not a fix to this program; it is a different one.

### What would change the answer

For dry/wet, someone wanting parallel processing where the plugins involved have
no mix control of their own. For the spectrum, a problem that peak and RMS per
plugin demonstrably cannot diagnose. For out-of-process hosting, nothing short of
deciding to build a different application; a specific plugin crashing repeatedly
would be worth removing from the chain rather than worth sandboxing.

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
strictly larger than the `AudioEngine` extraction declined above, which the
backlog itself warned was the riskiest refactor in the project.

**3. JUCE exposes no loopback.** `WASAPIDeviceMode` is `shared`, `exclusive` and
`sharedLowLatency`; the string `loopback` appears nowhere in
`juce_audio_devices`. Windows itself has supported loopback since Vista
(`AUDCLNT_STREAMFLAGS_LOOPBACK` on a render endpoint) and added a per-process API
in Windows 10 2004, so this is a framework gap rather than a platform one — see
the standing-checks note in BACKLOG.md. Without it, the reference has to
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
