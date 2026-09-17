#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cmath>

//==============================================================================
// Level metering for the signal path.
//
// WHY THIS EXISTS
//
// On 2026-09-15 the microphone sat at +30 dB hitting 0.0 dBFS for hours. The
// application showed nothing, so the first indication was colleagues reporting
// that the audio was unusable, and establishing what had happened took an hour
// of external capture and analysis. Every figure needed to diagnose it was
// passing through this process the whole time.
//
// WHAT IT COSTS, AND WHY THAT SHAPED THE DESIGN
//
// Light Host is tray-resident and its audio graph runs whenever a device is
// open, so "idle" still means every block is being processed. A meter that
// measured everything all the time would therefore burn CPU forever to feed a
// window that is shut, which is not a promise this application gets to break
// (see the note in LookAndFeel.hpp).
//
// So the work is split by what it is for:
//
//   Peak   always measured, via FloatVectorOperations::findMinAndMax, which is
//          SIMD. This is what feeds the clip latch, and a clip indicator that
//          only works while you are looking at it is pointless. Roughly 96k
//          samples per second scanned at 48 kHz stereo, several floats per
//          instruction: unmeasurable.
//
//   RMS    only while something is watching. The per-sample sum of squares is
//          the expensive half, and only the signal view displays it, so a
//          watcher count gates it. Hold a Meter::Watch to ask for it.
//
// WHO OWNS A METER
//
// Nothing that can be destroyed while the UI is looking at it. The device meters
// belong to DeviceTap, which outlives every window. The probe meters belong to
// IconMenu for the same reason, and a Probe borrows one by reference rather than
// owning it.
//
// That is not a stylistic preference. When Probe owned its Meter, reloading the
// chain destroyed every Probe through graph.clear() and recreated them, while the
// signal view went on reading the old pointers at 25 Hz and decrementing their
// watch counts afterwards. Adding a plugin and pressing Apply with the panel open
// was a use-after-free. A meter that outlives the graph cannot reproduce it.
//
// REALTIME RULES
//
// Meter::measure runs on the audio thread. It allocates nothing, takes no
// locks, and does no I/O. Everything crossing between threads is a relaxed
// atomic: a meter that stalls the audio thread to report a level is worse than
// no meter.
//==============================================================================
namespace lighthost::metering
{
    // The realtime rules above rest on these, so they are asserted rather than
    // assumed. A std::atomic that is not lock-free compiles to a mutex, which
    // would make "takes no locks" false on the audio thread with nothing
    // reporting it -- and the failure would be an audible dropout under load,
    // not a diagnostic. True on every platform this ships to; checked so that
    // stays a fact rather than a convention.
    static_assert (std::atomic<float>::is_always_lock_free,
                   "Meter publishes levels from the audio thread through "
                   "std::atomic<float>. If that is not lock-free here, metering "
                   "takes a lock on the audio thread.");
    static_assert (std::atomic<bool>::is_always_lock_free,
                   "Meter latches clipping from the audio thread through "
                   "std::atomic<bool>.");
    static_assert (std::atomic<int>::is_always_lock_free,
                   "Meter counts watchers through std::atomic<int>, decremented "
                   "from the message thread while the audio thread reads it.");

    /** Treated as full scale. Slightly below 1.0 so a converter that lands one
        LSB short of the rail still reads as clipped, which is what it is.
    */
    static constexpr float kClipThreshold = 0.999f;

    /** Smoothing for the mean-square estimate, per block. Fast enough to follow
        speech, slow enough that the number is readable rather than flickering.
    */
    static constexpr float kRmsCoefficient = 0.30f;

    /** How fast a held peak falls. THE one owner of that rate.

        Fast enough to follow a level down, slow enough that a transient stays
        readable for a moment after it has gone.

        It is here rather than beside either of the two things that fall,
        because up to 5.2.0 it was written out twice in different units -- as
        the per-block factor below, and again in PreferencesWindow.cpp as a
        per-tick dB step -- with both comments claiming 45 dB per second and
        nothing holding them to it. Drift means the bar the UI draws falls at a
        different rate from the peak it is drawing, so bars sag below the held
        peak or stick above it. That is the symptom that made 5.1.0 move the
        ballistics into Meter in the first place.
    */
    static constexpr float kPeakFallDbPerSecond = 45.0f;

    /** The block size and sample rate kPeakDecayPerBlock is quoted against.

        A per-block factor is only a rate once you say how long a block is, and
        that number belongs in code rather than in prose: this pair was a
        sentence in a comment, so nothing could check it. Not the real block
        size or rate, which are whatever the device gives us -- only the
        reference the literal below was chosen at. Tests/SignalMeteringTests.cpp
        reads them.
    */
    static constexpr int    kReferenceBlockSize  = 480;
    static constexpr double kReferenceSampleRate = 48000.0;

    /** kPeakFallDbPerSecond expressed as a per-block linear factor, which is
        what the audio thread can apply without a log.

        A literal rather than a derivation, because deriving it would need the
        reference pair above to be the truth about the running device, and it is
        not. Tests/SignalMeteringTests.cpp asserts the equivalence instead, so
        changing one rate without the other fails a test rather than quietly
        desynchronising the UI from the meter.
    */
    static constexpr float kPeakDecayPerBlock = 0.9496f;

    /** Reported when there is no signal at all, and the floor for every dB
        figure so a silent meter reads as a number rather than -inf.
    */
    static constexpr float kFloorDb = -100.0f;

    //==========================================================================
    /** One measurement point.

        Written by the audio thread, read by the message thread.

        Reading is NON-DESTRUCTIVE, and the ballistics live here rather than in
        the components that draw them. Both matter: the device meters have two
        readers whenever the signal view is open -- the meter under the device
        and the matching row of the column -- and when read() cleared the peak,
        each reader stole what the other would have shown, so both sagged and
        flickered. The audio thread holds a decaying peak instead, and any number
        of readers can observe the same one.
    */
    class Meter
    {
    public:
        /** Declared explicitly because the non-copyable macro below declares a
            deleted copy constructor, and any user-declared constructor
            suppresses the implicit default one.
        */
        Meter() = default;

        struct Reading
        {
            float peakDb  = kFloorDb;   ///< held peak, already decayed
            float rmsDb   = kFloorDb;
            bool  clipped = false;      ///< latched until clearClip()
            bool  rmsValid = false;     ///< false when nothing is watching
        };

        //----------------------------------------------------------------------
        /** Holds a watcher for as long as it exists, so RMS is measured.

            RAII because the alternative is a visible component that forgot to
            decrement, which would leave the expensive half running for the rest
            of the session with nothing to show for it.
        */
        class Watch
        {
        public:
            explicit Watch (Meter* m) : meter (m)
            {
                if (meter != nullptr)
                    meter->watchers.fetch_add (1, std::memory_order_relaxed);
            }

            ~Watch()
            {
                if (meter != nullptr)
                    meter->watchers.fetch_sub (1, std::memory_order_relaxed);
            }

            Watch (Watch&& other) noexcept : meter (other.meter) { other.meter = nullptr; }

            Watch (const Watch&)            = delete;
            Watch& operator= (const Watch&) = delete;
            Watch& operator= (Watch&&)      = delete;

        private:
            Meter* meter = nullptr;
        };

        /** True while at least one Watch exists. For tests and assertions. */
        [[nodiscard]] bool isWatched() const noexcept
        {
            return watchers.load (std::memory_order_relaxed) > 0;
        }

        //----------------------------------------------------------------------
        /** Audio thread. Raw channel pointers, as the device callback supplies. */
        void measure (const float* const* channels, int numChannels, int numSamples) noexcept
        {
            if (channels == nullptr || numChannels <= 0 || numSamples <= 0)
                return;

            const bool wantRms = watchers.load (std::memory_order_relaxed) > 0;

            float peak = 0.0f;
            float loudestChannelMeanSquare = 0.0f;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* data = channels[ch];

                if (data == nullptr)
                    continue;

                // SIMD, and the only work done when nothing is watching.
                const auto range = juce::FloatVectorOperations::findMinAndMax (data, numSamples);
                peak = juce::jmax (peak, std::abs (range.getStart()), std::abs (range.getEnd()));

                if (! wantRms)
                    continue;

                double sumOfSquares = 0.0;

                for (int i = 0; i < numSamples; ++i)
                    sumOfSquares += static_cast<double> (data[i]) * data[i];

                // The LOUDEST channel, not the average across them. A mono
                // signal wired into a stereo probe leaves one channel silent,
                // and averaging would report it 3 dB quieter than it is.
                loudestChannelMeanSquare =
                    juce::jmax (loudestChannelMeanSquare,
                                static_cast<float> (sumOfSquares / numSamples));
            }

            publish (peak, loudestChannelMeanSquare, wantRms);
        }

        /** Audio thread. The buffer form, for a processor in the graph. */
        void measure (const juce::AudioBuffer<float>& buffer) noexcept
        {
            measure (buffer.getArrayOfReadPointers(),
                     buffer.getNumChannels(),
                     buffer.getNumSamples());
        }

        //----------------------------------------------------------------------
        /** Any thread. Does not modify anything, so two components reading the
            same meter both see the whole picture.
        */
        [[nodiscard]] Reading read() const noexcept
        {
            const auto peak = heldPeak.load (std::memory_order_relaxed);
            const auto meanSquare = smoothedMeanSquare.load (std::memory_order_relaxed);

            Reading r;
            r.peakDb   = juce::Decibels::gainToDecibels (peak, kFloorDb);
            r.rmsValid = rmsMeasured.load (std::memory_order_relaxed);
            r.rmsDb    = r.rmsValid
                             ? juce::Decibels::gainToDecibels (std::sqrt (meanSquare), kFloorDb)
                             : kFloorDb;
            r.clipped  = clipLatched.load (std::memory_order_relaxed);
            return r;
        }

        /** The clip badge latches, so it needs clearing. */
        void clearClip() noexcept
        {
            clipLatched.store (false, std::memory_order_relaxed);
        }

        /** Forgets everything. Called on a device change, and when a probe stops
            existing so its row reads silence rather than a frozen level.

            Safe from any thread: every member is a relaxed atomic.
        */
        void reset() noexcept
        {
            heldPeak.store (0.0f, std::memory_order_relaxed);
            smoothedMeanSquare.store (0.0f, std::memory_order_relaxed);
            clipLatched.store (false, std::memory_order_relaxed);
            rmsMeasured.store (false, std::memory_order_relaxed);
        }

        /** Points the peak decay at the block size and rate actually in use.

            Until 5.4.0 the decay was the fixed literal kPeakDecayPerBlock, which
            is 45 dB/s at 480 samples and 48 kHz and at NO other pair. The UI half
            falls by kPeakFallDbPerSecond/refreshHz per tick, which is a true
            45 dB/s at any buffer size, so the two agreed only at the reference --
            and buffer size is a setting the user picks in Preferences. Measured
            at the fixed literal: 128 samples at 48 kHz fell at about 169 dB/s,
            and 1024 at 44.1 kHz at about 19 dB/s. What the user sees is the
            slower of the two, so on a large buffer the peak bar sticks above a
            level that has already gone -- verbatim the symptom the shared
            constant was introduced to prevent, and the one that made 5.1.0 move
            the ballistics in here.

            Call from prepareToPlay or audioDeviceAboutToStart, where JUCE has
            the graph stopped. `decayPerBlock` is a plain float for that reason,
            the same argument GainProcessor makes for rampTargetDb: the audio
            thread is not running when this is written.

            A non-positive rate or block size keeps the reference literal rather
            than producing a nonsense factor, because a meter that decays wrongly
            is better than one that never decays or decays to NaN.
        */
        void setTimebase (double sampleRate, int blockSize) noexcept
        {
            if (sampleRate <= 0.0 || blockSize <= 0)
            {
                decayPerBlock = kPeakDecayPerBlock;
                return;
            }

            const auto secondsPerBlock = static_cast<double> (blockSize) / sampleRate;
            const auto fallDb          = kPeakFallDbPerSecond * secondsPerBlock;

            decayPerBlock = static_cast<float> (std::pow (10.0, -fallDb / 20.0));
        }

        /** The factor in use, so a test can check the rate rather than trust it. */
        [[nodiscard]] float getDecayPerBlock() const noexcept { return decayPerBlock; }

    private:
        void publish (float peak, float channelMeanSquare, bool measuredRms) noexcept
        {
            // Decay held here rather than in the UI, so every reader sees the
            // same ballistics and none can take the peak away from another.
            const auto decayed = heldPeak.load (std::memory_order_relaxed) * decayPerBlock;
            heldPeak.store (juce::jmax (peak, decayed), std::memory_order_relaxed);

            if (measuredRms)
            {
                const auto previous = smoothedMeanSquare.load (std::memory_order_relaxed);

                smoothedMeanSquare.store (
                    previous + kRmsCoefficient * (channelMeanSquare - previous),
                    std::memory_order_relaxed);
            }
            else
            {
                // The ACCUMULATOR is zeroed while nothing is watching, not just
                // the flag below. Clearing the flag hides the old figure; it
                // does not remove it, and the smoother is recursive, so with
                // kRmsCoefficient at 0.30 the first block after a watcher came
                // back read 70% of whatever was in here when the last one left.
                // Open the signal view with audio playing, close it, silence
                // the input and reopen: a phantom level for ~100 ms, sourced
                // from audio that had stopped.
                //
                // One relaxed store on a path that is already doing a store, so
                // the realtime rules at the top of this file still hold.
                smoothedMeanSquare.store (0.0f, std::memory_order_relaxed);
            }

            // Cleared as soon as nothing is watching, so a reader that arrives
            // later is not shown a frozen figure from the last time it was.
            rmsMeasured.store (measuredRms, std::memory_order_relaxed);

            if (peak >= kClipThreshold)
                clipLatched.store (true, std::memory_order_relaxed);
        }

        /** Peak decay per block, for the rate and block size actually running.

            Not an atomic, deliberately. It is written only by setTimebase,
            which its own doc restricts to prepareToPlay and
            audioDeviceAboutToStart -- both called by JUCE with the graph
            stopped, so no audio thread is reading it at the time. Same rule
            GainProcessor::rampTargetDb follows.
        */
        float decayPerBlock = kPeakDecayPerBlock;

        std::atomic<float> heldPeak           { 0.0f };
        std::atomic<float> smoothedMeanSquare { 0.0f };
        std::atomic<bool>  clipLatched        { false };
        std::atomic<bool>  rmsMeasured        { false };
        std::atomic<int>   watchers           { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Meter)
    };

    //==========================================================================
    /** A pass-through processor that measures whatever goes through it.

        Inserted after a plugin while the signal view is open. It reports no
        latency and touches no samples, so adding one cannot change what the
        chain sounds like or disturb the graph's inter-lane delay compensation --
        the same property that lets the lane trims sit in the path.

        It BORROWS its meter. The graph owns the processor and destroys it on
        every chain reload; the meter has to outlive that, because the UI holds a
        pointer to it. See the ownership note at the top of this file.
    */
    class Probe final : public juce::AudioProcessor
    {
    public:
        explicit Probe (Meter& meterToFill)
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
              meter (meterToFill)
        {
        }

        const juce::String getName() const override        { return "Signal Probe"; }
        bool acceptsMidi() const override                  { return false; }
        bool producesMidi() const override                 { return false; }
        double getTailLengthSeconds() const override       { return 0.0; }
        int getNumPrograms() override                       { return 1; }
        int getCurrentProgram() override                    { return 0; }
        void setCurrentProgram (int) override               {}
        const juce::String getProgramName (int) override    { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        bool hasEditor() const override                     { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }

        // Nothing to persist: a probe's only state is a level, and a level is
        // not worth saving. Empty rather than absent because AudioProcessor
        // makes them pure virtual.
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override  {}

        /** Both arguments are used now, and both were discarded before. The
            peak decay is per block, so it is not a rate until it knows how long
            a block is -- see Meter::setTimebase for what the fixed literal cost
            at every buffer size other than the reference one.
        */
        void prepareToPlay (double sampleRate, int samplesPerBlock) override
        {
            meter.setTimebase (sampleRate, samplesPerBlock);
            meter.reset();
        }
        void releaseResources() override                     {}

        using juce::AudioProcessor::processBlock;

        /** noexcept because Meter::measure is, and it is the only thing called.

            Narrowing an override this way is a promise the audio thread can
            use, and it is safe HERE precisely because nothing third-party is
            reachable from it. DeviceTap's device callback deliberately keeps
            the opposite promise: it forwards into plugin code, where noexcept
            would convert a plugin's throw into std::terminate.
        */
        void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer&) noexcept override
        {
            meter.measure (audio);
        }

    private:
        Meter& meter;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Probe)
    };
}
