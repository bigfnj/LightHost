#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class DelayProcessor final : public juce::AudioProcessor
{
public:
    explicit DelayProcessor (int delaySamples)
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          delayInSamples (delaySamples)
    {
    }

    ~DelayProcessor() override = default;

    const juce::String getName() const override { return "PDC Delay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override
    {
        // Clamp to 10 seconds of audio — guards against misbehaving plugins reporting
        // absurd latency values that would otherwise trigger gigabyte allocations.
        const int maxAllowed = static_cast<int> (sampleRate * 10.0);
        delayInSamples       = juce::jlimit (0, maxAllowed, delayInSamples);
        const int bufSize    = juce::jmax (maximumExpectedSamplesPerBlock, delayInSamples + 1);
        buffer.setSize (2, bufSize);
        buffer.clear();
        writePos = 0;
    }

    void releaseResources() override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    // Expose base-class processBlock(double&) overload so it isn't hidden by our
    // float override.  We don't implement double-precision processing — JUCE's
    // graph runs in float by default — but this silences the -Woverloaded-virtual.
    using juce::AudioProcessor::processBlock;

    void processBlock (juce::AudioBuffer<float>& data, juce::MidiBuffer&) override
    {
        if (delayInSamples <= 0) return;

        const int numChannels = juce::jmin (data.getNumChannels(), buffer.getNumChannels());
        const int numSamples  = data.getNumSamples();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* channelData   = data.getWritePointer (ch);
            auto* delayData     = buffer.getWritePointer (ch);
            int localWritePos   = writePos;

            for (int i = 0; i < numSamples; ++i)
            {
                const float in = channelData[i];
                int readPos = localWritePos - delayInSamples;
                if (readPos < 0) readPos += buffer.getNumSamples();

                channelData[i] = delayData[readPos];
                delayData[localWritePos] = in;

                if (++localWritePos >= buffer.getNumSamples())
                    localWritePos = 0;
            }
        }
        writePos = (writePos + numSamples) % buffer.getNumSamples();
    }

private:
    int delayInSamples;
    juce::AudioBuffer<float> buffer;
    int writePos = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayProcessor)
};
