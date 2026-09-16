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
//          the expensive half and it is only ever read by a visible meter, so a
//          watcher count gates it. Use Meter::Watch to hold one.
//
// Probe is created only while the signal view is expanded, so per-plugin taps
// cost nothing the rest of the time. DeviceTap (in DeviceTap.hpp) is always
// active, because the input clip latch is the whole point.
//
// REALTIME RULES
//
// Meter::measure runs on the audio thread. It allocates nothing, takes no
// locks, and does no I/O. Everything crossing to the message thread is a
// relaxed atomic: a meter that stalls the audio thread to report a level is
// worse than no meter.
//==============================================================================
namespace lighthost::metering
{
    /** Treated as full scale. Slightly below 1.0 so a converter that lands one
        LSB short of the rail still reads as clipped, which is what it is.
    */
    static constexpr float kClipThreshold = 0.999f;

    /** Smoothing for the mean-square estimate, per block. Fast enough to follow
        speech, slow enough that the number is readable rather than flickering.
    */
    static constexpr float kRmsCoefficient = 0.30f;

    /** Reported when there is no signal at all, and the floor for every dB
        figure so a silent meter reads as a number rather than -inf.
    */
    static constexpr float kFloorDb = -100.0f;

    //==========================================================================
    /** One measurement point.

        Written by the audio thread, read by the message thread. The peak is a
        max-since-last-read: the audio thread raises it, read() takes it and
        clears it. That way a peak between two UI frames cannot be missed, which
        a plain "last block's peak" would lose two times out of three at any
        sensible refresh rate.
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
            float peakDb = kFloorDb;
            float rmsDb  = kFloorDb;
            bool  clipped = false;      ///< latched until clearClip()
            bool  active  = false;      ///< any signal at all since the last read
            bool  rmsValid = false;     ///< false when nothing was watching
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
        /** Message thread. Takes the peak accumulated since the previous call. */
        [[nodiscard]] Reading read() noexcept
        {
            const auto peak = peakSinceRead.exchange (0.0f, std::memory_order_relaxed);
            const auto meanSquare = smoothedMeanSquare.load (std::memory_order_relaxed);
            const auto seen = sawAudio.exchange (false, std::memory_order_relaxed);

            Reading r;
            r.peakDb   = juce::Decibels::gainToDecibels (peak, kFloorDb);
            r.rmsValid = rmsMeasured.load (std::memory_order_relaxed);
            r.rmsDb    = r.rmsValid ? juce::Decibels::gainToDecibels (std::sqrt (meanSquare), kFloorDb)
                                    : kFloorDb;
            r.clipped  = clipLatched.load (std::memory_order_relaxed);
            r.active   = seen;
            return r;
        }

        /** Message thread. The clip badge latches, so it needs clearing. */
        void clearClip() noexcept
        {
            clipLatched.store (false, std::memory_order_relaxed);
        }

        /** Message thread. Forgets everything, for a device change. */
        void reset() noexcept
        {
            peakSinceRead.store (0.0f, std::memory_order_relaxed);
            smoothedMeanSquare.store (0.0f, std::memory_order_relaxed);
            clipLatched.store (false, std::memory_order_relaxed);
            sawAudio.store (false, std::memory_order_relaxed);
            rmsMeasured.store (false, std::memory_order_relaxed);
        }

    private:
        void publish (float peak, float channelMeanSquare, bool measuredRms) noexcept
        {
            // Raise the held peak without ever lowering it; read() does the
            // lowering. compare_exchange rather than a plain store, because two
            // graph threads could in principle reach the same meter.
            auto held = peakSinceRead.load (std::memory_order_relaxed);

            while (peak > held
                   && ! peakSinceRead.compare_exchange_weak (held, peak,
                                                             std::memory_order_relaxed))
            {
            }

            if (measuredRms)
            {
                const auto previous = smoothedMeanSquare.load (std::memory_order_relaxed);

                smoothedMeanSquare.store (
                    previous + kRmsCoefficient * (channelMeanSquare - previous),
                    std::memory_order_relaxed);

                rmsMeasured.store (true, std::memory_order_relaxed);
            }

            if (peak >= kClipThreshold)
                clipLatched.store (true, std::memory_order_relaxed);

            if (peak > 0.0f)
                sawAudio.store (true, std::memory_order_relaxed);
        }

        std::atomic<float> peakSinceRead      { 0.0f };
        std::atomic<float> smoothedMeanSquare { 0.0f };
        std::atomic<bool>  clipLatched        { false };
        std::atomic<bool>  sawAudio           { false };
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
    */
    class Probe final : public juce::AudioProcessor
    {
    public:
        Probe()
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
        {
        }

        [[nodiscard]] Meter& getMeter() noexcept { return meter; }

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

        void prepareToPlay (double, int) override            { meter.reset(); }
        void releaseResources() override                     {}

        using juce::AudioProcessor::processBlock;

        void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
        {
            meter.measure (audio);
        }

    private:
        Meter meter;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Probe)
    };
}
