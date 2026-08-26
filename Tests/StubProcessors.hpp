#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// Stub processors for offline graph tests. No real plugin, no audio device.
//==============================================================================
namespace lighthost::test
{
    /** A faithful plugin: reports N samples of latency AND actually delays by N.

        This is what a well-behaved plugin does, and it is the honest model to
        test the host against. Reporting latency you do not incur, or incurring
        latency you do not report, are both separate (and interesting) cases —
        see LyingLatencyStub.
    */
    class LatencyStub final : public juce::AudioProcessor
    {
    public:
        explicit LatencyStub (int latencySamples)
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
              latency (latencySamples)
        {
            setLatencySamples (latency);
        }

        const juce::String getName() const override        { return "LatencyStub"; }
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
        void releaseResources() override                    {}

        void prepareToPlay (double, int maximumExpectedSamplesPerBlock) override
        {
            setLatencySamples (latency);
            buffer.setSize (2, juce::jmax (maximumExpectedSamplesPerBlock, latency + 1));
            buffer.clear();
            writePos = 0;
        }

        using juce::AudioProcessor::processBlock;

        void processBlock (juce::AudioBuffer<float>& data, juce::MidiBuffer& midi) override
        {
            applyDelay (data, midi);
        }

        /** A plugin that reports latency MUST produce the same latency when
            bypassed, or the host's compensation shifts its audio forward in time.
            JUCE asserts on this in AudioProcessor::processBypassed. Honouring the
            contract here is what makes this stub a fair model of a real plugin.
        */
        void processBlockBypassed (juce::AudioBuffer<float>& data, juce::MidiBuffer& midi) override
        {
            applyDelay (data, midi);
        }

    private:
        void applyDelay (juce::AudioBuffer<float>& data, juce::MidiBuffer&)
        {
            if (latency <= 0) return;

            const int numChannels = juce::jmin (data.getNumChannels(), buffer.getNumChannels());
            const int numSamples  = data.getNumSamples();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* io    = data.getWritePointer (ch);
                auto* delay = buffer.getWritePointer (ch);
                int   pos   = writePos;

                for (int i = 0; i < numSamples; ++i)
                {
                    const float in = io[i];
                    int readPos = pos - latency;
                    if (readPos < 0) readPos += buffer.getNumSamples();

                    io[i]      = delay[readPos];
                    delay[pos] = in;

                    if (++pos >= buffer.getNumSamples())
                        pos = 0;
                }
            }

            writePos = (writePos + numSamples) % buffer.getNumSamples();
        }

        int latency;
        juce::AudioBuffer<float> buffer;
        int writePos = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LatencyStub)
    };

    /** Passes audio through with a chosen number of input and output channels.

        For wiring tests: a real chain contains mono plugins, and the host has to
        wire them from the channel counts the processor actually reports rather
        than from what a connection attempt returns. Zero channels on a side means
        that bus is absent, which models a plugin whose bus negotiation failed.
    */
    class ChannelStub final : public juce::AudioProcessor
    {
    public:
        ChannelStub (int numIns, int numOuts)
            : AudioProcessor (busesFor (numIns, numOuts))
        {
        }

        const juce::String getName() const override        { return "ChannelStub"; }
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
        void prepareToPlay (double, int) override            {}
        void releaseResources() override                     {}

        using juce::AudioProcessor::processBlock;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}

    private:
        static BusesProperties busesFor (int numIns, int numOuts)
        {
            BusesProperties buses;

            if (numIns > 0)
                buses = buses.withInput ("In", juce::AudioChannelSet::canonicalChannelSet (numIns), true);

            if (numOuts > 0)
                buses = buses.withOutput ("Out", juce::AudioChannelSet::canonicalChannelSet (numOuts), true);

            return buses;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStub)
    };

    /** Passes audio straight through, no latency, no reporting. */
    class PassthroughStub final : public juce::AudioProcessor
    {
    public:
        PassthroughStub()
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override        { return "PassthroughStub"; }
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
        void prepareToPlay (double, int) override            {}
        void releaseResources() override                     {}

        using juce::AudioProcessor::processBlock;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PassthroughStub)
    };
}
