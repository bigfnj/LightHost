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
// processBlock runs on the audio thread. It is not the only code here that
// does -- Meter::measure, Probe::processBlock and DeviceTap's device callback
// all do too since 5.1.0, and Source/SignalMetering.hpp states the same rules
// for them. It was the only one when this was written. It allocates nothing, takes no locks,
// and does no I/O. The setting comes across from the message thread as ONE
// relaxed atomic, in decibels, and is ramped, because a gain applied as a step
// produces a click, and a click is what a user will report as a bug in the
// plugin they were listening to. Why one atomic and not a decibel value beside
// a linear one is in processBlock, and it is a correctness point rather than a
// tidiness one.
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

    /** True when a trim is close enough to unity that it can be skipped.

        The tolerance is in DECIBELS, so the caller has to be holding decibels.
        processBlock is the only caller and asks it directly; a linear test of
        its own would be a second rule, and the two do not agree.
    */
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
            currentDb.store (clampDb (decibels), std::memory_order_relaxed);
        }

        /** The trim in decibels, as clamped. For tests: nothing shipped reads it
            back from here, because IconMenu::getLaneGainDb answers from the
            chain store rather than from the graph. Kept rather than deleted
            because the clamp above is otherwise observable only by rendering,
            which would measure the ramp at the same time and no longer be a
            test of the clamp.
        */
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
            rampTargetDb = currentDb.load (std::memory_order_relaxed);
            smoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (rampTargetDb, kMinDb));
        }

        using juce::AudioProcessor::processBlock;

        void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
        {
            const auto db = currentDb.load (std::memory_order_relaxed);

            // The decibels are converted here rather than being carried across
            // as a second, linear atomic beside them. Two atomics written one
            // after the other can be read apart: a block landing between the
            // stores sees the new decibels next to the previous linear target,
            // and if the ramp has already settled on that older target the skip
            // below fires -- passing a lane the user has just unmuted at full
            // level for one block. One value cannot disagree with itself.
            //
            // The conversion is a pow, and it runs only when the setting has
            // moved, so at most once per block while a fader is dragged. It
            // allocates nothing and takes no lock, so the rules at the top of
            // this file still hold.
            if (! juce::approximatelyEqual (db, rampTargetDb))
            {
                rampTargetDb = db;
                smoothed.setTargetValue (juce::Decibels::decibelsToGain (db, kMinDb));
            }

            // Settled at unity: the common case, and worth not touching the
            // buffer for. Asked of isUnity in DECIBELS, not re-derived as a
            // linear comparison here, because the two rules do not agree:
            // isUnity (0.0005f) is true and decibelsToGain (0.0005f) is not
            // approximatelyEqual to 1.0f. The skip therefore discards at most
            // isUnity's tolerance, 0.001 dB, which is what that tolerance is
            // for.
            if (! smoothed.isSmoothing() && isUnity (db))
                return;

            smoothed.applyGain (audio, audio.getNumSamples());
        }

    private:
        /** The setting, and the only copy of it. Read every block. */
        std::atomic<float> currentDb { kDefaultDb };

        /** What the ramp was last pointed at, so the conversion happens on a
            change rather than every block. Audio thread only, except in
            prepareToPlay, which JUCE calls with the graph stopped -- the same
            condition that lets prepareToPlay touch `smoothed`.
        */
        float rampTargetDb = kDefaultDb;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothed;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Processor)
    };
}
