#include "../Source/GainProcessor.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

//==============================================================================
// The lane trim, rendered rather than reasoned about.
//==============================================================================
namespace
{
    constexpr int    kBlockSize  = 128;
    constexpr double kSampleRate = 48000.0;

    /** Renders a constant signal and returns every output sample of channel 0. */
    std::vector<float> renderConstant (lighthost::gain::Processor& processor,
                                       float inputLevel,
                                       int numBlocks)
    {
        juce::AudioBuffer<float> audio (2, kBlockSize);
        juce::MidiBuffer midi;

        std::vector<float> out;
        out.reserve (static_cast<size_t> (kBlockSize) * static_cast<size_t> (numBlocks));

        for (int block = 0; block < numBlocks; ++block)
        {
            for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                juce::FloatVectorOperations::fill (audio.getWritePointer (channel),
                                                   inputLevel, kBlockSize);

            processor.processBlock (audio, midi);

            for (int i = 0; i < kBlockSize; ++i)
                out.push_back (audio.getSample (0, i));
        }

        return out;
    }

    /** A prepared processor. Held rather than returned: an AudioProcessor is not
        copyable or movable, so it cannot be handed back from a factory function.
    */
    struct PreparedGain
    {
        explicit PreparedGain (float decibels)
        {
            processor.setGainDb (decibels);
            processor.prepareToPlay (kSampleRate, kBlockSize);
        }

        lighthost::gain::Processor processor;
    };
}

//==============================================================================
class GainProcessorTests final : public juce::UnitTest
{
public:
    GainProcessorTests()
        : juce::UnitTest ("Lane gain", "Gain") {}

    void runTest() override
    {
        using namespace lighthost::gain;

        beginTest ("unity passes the signal through untouched");
        {
            Processor processor;
            processor.prepareToPlay (kSampleRate, kBlockSize);

            const auto rendered = renderConstant (processor, 0.5f, 2);

            for (const auto sample : rendered)
                expectWithinAbsoluteError (sample, 0.5f, 1.0e-6f);
        }

        beginTest ("minus six decibels halves the amplitude");
        {
            PreparedGain gain (-6.020599913f);
            const auto rendered = renderConstant (gain.processor, 1.0f, 2);

            // The ramp is settled by construction: prepareToPlay starts at the
            // target rather than ramping from unity.
            expectWithinAbsoluteError (rendered.back(), 0.5f, 1.0e-4f);
        }

        beginTest ("plus six decibels doubles it");
        {
            PreparedGain gain (6.020599913f);
            const auto rendered = renderConstant (gain.processor, 0.25f, 2);

            expectWithinAbsoluteError (rendered.back(), 0.5f, 1.0e-4f);
        }

        beginTest ("the range is clamped, and silly values cannot get in");
        {
            Processor processor;

            processor.setGainDb (1000.0f);
            expectEquals (processor.getGainDb(), kMaxDb);

            processor.setGainDb (-1000.0f);
            expectEquals (processor.getGainDb(), kMinDb);

            processor.setGainDb (-3.0f);
            expectEquals (processor.getGainDb(), -3.0f);
        }

        beginTest ("a change while running ramps instead of stepping");
        {
            // A gain applied as a step is a click, and a click gets reported as a
            // bug in whichever plugin happened to be playing.
            PreparedGain gain (0.0f);

            renderConstant (gain.processor, 1.0f, 1);          // settled at unity
            gain.processor.setGainDb (kMinDb);                 // hard duck, mid-stream
            const auto rendered = renderConstant (gain.processor, 1.0f, 1);

            // The first sample after the change must not have jumped the whole
            // way. A step would land at the new value immediately.
            expect (rendered.front() > 0.5f,
                    "the gain changed as a step rather than a ramp");

            // The largest jump between neighbouring samples bounds the click.
            float largestStep = 0.0f;

            for (size_t i = 1; i < rendered.size(); ++i)
                largestStep = juce::jmax (largestStep, std::abs (rendered[i] - rendered[i - 1]));

            expect (largestStep < 0.01f,
                    "sample-to-sample jump of " + juce::String (largestStep)
                        + " is large enough to hear");
        }

        beginTest ("a ramp reaches its target and stays there");
        {
            PreparedGain gain (0.0f);
            renderConstant (gain.processor, 1.0f, 1);
            gain.processor.setGainDb (-6.020599913f);

            // kRampSeconds at kSampleRate is well inside eight blocks.
            const auto rendered = renderConstant (gain.processor, 1.0f, 8);

            expectWithinAbsoluteError (rendered.back(), 0.5f, 1.0e-4f);
        }

        beginTest ("prepareToPlay does not fade the lane in");
        {
            // A device change re-prepares every node. Ramping up from unity there
            // would duck or swell the lane for 20ms on every device switch.
            PreparedGain gain (-6.020599913f);
            const auto rendered = renderConstant (gain.processor, 1.0f, 1);

            expectWithinAbsoluteError (rendered.front(), 0.5f, 1.0e-3f);
        }

        beginTest ("unity is recognised, and near-unity is not");
        {
            expect (isUnity (0.0f));
            expect (! isUnity (0.5f));
            expect (! isUnity (-0.5f));
        }
    }
};

static GainProcessorTests gainProcessorTests;
