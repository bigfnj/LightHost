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
            blockSize = maximumExpectedSamplesPerBlock;
            buffer.setSize (2, juce::jmax (maximumExpectedSamplesPerBlock, latency + 1));
            buffer.clear();
            writePos = 0;
        }

        /** Changes the reported latency the way a plugin does when the user
            switches it to a linear-phase or oversampled mode inside its editor.
            Calls updateHostDisplay, so a listening host hears about it.
        */
        void changeLatencyTo (int newLatency)
        {
            latency = newLatency;
            setLatencySamples (latency);
            buffer.setSize (2, juce::jmax (blockSize, latency + 1));
            buffer.clear();
            writePos = 0;
            updateHostDisplay (ChangeDetails{}.withLatencyChanged (true));
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
        int blockSize = 0;
        juce::AudioBuffer<float> buffer;
        int writePos = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LatencyStub)
    };

    /** A latency-reporting plugin that exposes its own bypass parameter.

        This is the other half of bypass handling, and the half that is easy to
        get wrong. The graph only bypasses a node itself when the processor has no
        bypass parameter (juce_AudioProcessorGraph.cpp:893). When it has one,
        setBypassed just sets that parameter: the plugin keeps being called and
        keeps its latency, so the host must keep compensating for it. A host that
        treated a bypassed plugin as zero latency would pull that lane out of time
        with the others, which is silent phase cancellation rather than an obvious
        failure.
    */
    class BypassParameterStub final : public juce::AudioProcessor
    {
    public:
        explicit BypassParameterStub (int latencySamples)
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
              delay (latencySamples)
        {
            addParameter (bypass = new juce::AudioParameterBool ({ "bypass", 1 }, "Bypass", false));
            setLatencySamples (latencySamples);
        }

        juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

        const juce::String getName() const override        { return "BypassParameterStub"; }
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

        void prepareToPlay (double rate, int maximumExpectedSamplesPerBlock) override
        {
            delay.prepareToPlay (rate, maximumExpectedSamplesPerBlock);
        }

        using juce::AudioProcessor::processBlock;

        void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
        {
            // Delays either way. A plugin that reports latency must produce it
            // whether its own bypass is engaged or not.
            delay.processBlock (audio, midi);
        }

    private:
        LatencyStub delay;
        juce::AudioParameterBool* bypass = nullptr;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BypassParameterStub)
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
