#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

//==============================================================================
// A trim for one parallel lane.
//
// WHY THIS EXISTS
//
// Lanes are fed the same input and summed at the graph's output node, so four
// lanes carrying near-identical material sum coherently: four times the amplitude
// is +12.04 dB, and two lanes is +6.02 dB. Up to 4.0.3 there was no trim, no
// meter and no dry/wet, so using the feature as intended made the output clip and
// the only remedy was to turn something else down.
//
// REALTIME RULES
//
// processBlock is the only realtime-critical code in this project now that the
// hand-rolled delay compensation is gone. It allocates nothing, takes no locks,
// and does no I/O. The target comes across from the message thread as a relaxed
// atomic and is ramped, because a gain applied as a step produces a click, and a
// click is what a user will report as a bug in the plugin they were listening to.
//
// The processor reports no latency, so inserting one does not disturb the graph's
// inter-lane delay compensation.
//==============================================================================
namespace lighthost::gain
{
    /** Range of the lane trim, in decibels. kMinDb is passed to
        Decibels::decibelsToGain as its minus-infinity point, so the bottom of the
        range is a true mute rather than a very quiet lane.
    */
    static constexpr float kMinDb     = -60.0f;
    static constexpr float kMaxDb     =  12.0f;
    static constexpr float kDefaultDb =   0.0f;

    /** Ramp length. Long enough to be inaudible as a step, short enough that a
        fader still feels immediate.
    */
    static constexpr double kRampSeconds = 0.02;

    [[nodiscard]] inline float clampDb (float decibels) noexcept
    {
        return juce::jlimit (kMinDb, kMaxDb, decibels);
    }

    /** True when a trim is close enough to unity that it can be skipped. */
    [[nodiscard]] inline bool isUnity (float decibels) noexcept
    {
        return std::abs (decibels) < 0.001f;
    }

    //==========================================================================
    class Processor final : public juce::AudioProcessor
    {
    public:
        Processor()
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
        {
            smoothed.setCurrentAndTargetValue (1.0f);
        }

        /** Sets the trim. Safe to call from the message thread while audio runs. */
        void setGainDb (float decibels)
        {
            const auto clamped = clampDb (decibels);
            currentDb.store (clamped, std::memory_order_relaxed);
            targetLinear.store (juce::Decibels::decibelsToGain (clamped, kMinDb),
                                std::memory_order_relaxed);
        }

        [[nodiscard]] float getGainDb() const
        {
            return currentDb.load (std::memory_order_relaxed);
        }

        //======================================================================
        const juce::String getName() const override        { return "Lane Gain"; }
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
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override  {}
        void releaseResources() override                     {}

        void prepareToPlay (double sampleRate, int) override
        {
            // Start at the target rather than ramping up from wherever the last
            // session left off: a device change should not fade the lane in.
            smoothed.reset (sampleRate, kRampSeconds);
            smoothed.setCurrentAndTargetValue (targetLinear.load (std::memory_order_relaxed));
        }

        using juce::AudioProcessor::processBlock;

        void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
        {
            const auto target = targetLinear.load (std::memory_order_relaxed);

            if (! juce::approximatelyEqual (target, smoothed.getTargetValue()))
                smoothed.setTargetValue (target);

            // Settled at unity: the common case, and worth not touching the
            // buffer for.
            if (! smoothed.isSmoothing() && juce::approximatelyEqual (target, 1.0f))
                return;

            smoothed.applyGain (audio, audio.getNumSamples());
        }

    private:
        std::atomic<float> targetLinear { 1.0f };
        std::atomic<float> currentDb    { kDefaultDb };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothed;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Processor)
    };
}
