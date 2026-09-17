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

            // One assertion over the largest departure, not 256 per-sample
            // ones. The 256 could not fail, and that was measured rather than
            // argued: with the unity skip in processBlock deleted,
            // smoothed.applyGain multiplies the buffer by exactly 1.0f and all
            // 256 samples still come back bit-identical, so no input told the
            // skipped path and the multiplied path apart here.
            //
            // What survives is the arithmetic claim -- a unity trim must not
            // scale the signal -- on the same 1.0e-6f threshold, so this fails
            // on exactly the inputs the loop failed on: the maximum departure
            // exceeds the tolerance precisely when some individual sample does.
            // The skip ITSELF is guarded by "the buffer is skipped on the
            // decibel rule, not a linear one" at the bottom of this file, which
            // is the only test that can distinguish the two paths.
            float largestDeparture = 0.0f;

            for (const auto sample : rendered)
                largestDeparture = juce::jmax (largestDeparture, std::abs (sample - 0.5f));

            expectWithinAbsoluteError (largestDeparture, 0.0f, 1.0e-6f,
                                       "a unity trim scaled the signal");
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

            // BOTH blocks are kept and joined, because the boundary between
            // them is where the change lands and so is the one neighbouring
            // pair that can carry a click. This used to discard the first
            // block's samples and walk only the second, which put that pair
            // outside the measured range: the largest-step bound below started
            // at the second sample after the change and never saw the first.
            // All that bounded the boundary was rendered.front() > 0.5f, which
            // tolerates a jump of 0.5 where the loop two lines further down
            // insists on 0.01 -- fifty times looser, at exactly the sample a
            // step would show up on.
            auto rendered = renderConstant (gain.processor, 1.0f, 1);   // settles at unity
            gain.processor.setGainDb (kMinDb);                          // hard duck, mid-stream

            const auto firstAfterChange = rendered.size();
            const auto secondBlock = renderConstant (gain.processor, 1.0f, 1);
            rendered.insert (rendered.end(), secondBlock.begin(), secondBlock.end());

            // The first sample after the change must not have jumped the whole
            // way. A step would land at the new value immediately.
            expect (rendered[firstAfterChange] > 0.5f,
                    "the gain changed as a step rather than a ramp");

            // The largest jump between neighbouring samples bounds the click,
            // and now spans the block boundary.
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

        beginTest ("the order PreparedGain uses is the order production uses");
        {
            // PreparedGain sets the trim and then prepares, and every test
            // above rests on that: prepareToPlay seeds the ramp from whatever
            // gain is set AT THAT MOMENT, so the order is the whole of the
            // no-fade-in guarantee.
            //
            // IconMenu::createLaneGainNodes used to do it the other way round
            // -- graph.addNode prepares the node, and the trim was applied
            // afterwards -- so production got the second half below while every
            // test got the first, and the fade the test above denies happened
            // on every chain load. The helper now matches production because
            // production was changed to match it.
            PreparedGain setThenPrepared (-6.020599913f);
            const auto correct = renderConstant (setThenPrepared.processor, 1.0f, 1);

            expectWithinAbsoluteError (correct.front(), 0.5f, 1.0e-3f,
                                       "a trim set before prepareToPlay must be live on the "
                                       "first sample");

            Processor preparedThenSet;
            preparedThenSet.prepareToPlay (kSampleRate, kBlockSize);
            preparedThenSet.setGainDb (-6.020599913f);
            const auto swept = renderConstant (preparedThenSet, 1.0f, 1);

            // kRampSeconds is 20 ms, so a 128-sample block at 48 kHz covers an
            // eighth of the ramp: the wrong order leaves the whole first block
            // between 6 dB too loud and 5.4 dB too loud. Asserted so that if
            // the fade ever stops happening, this stops claiming to describe
            // it rather than passing vacuously.
            expect (swept.front() > 0.99f,
                    "the wrong order is supposed to start at unity, and did not");
            expect (swept.back() > 0.9f,
                    "a whole block of the wrong order should still be nowhere near the trim");
        }

        beginTest ("unity is recognised, and near-unity is not");
        {
            expect (isUnity (0.0f));
            expect (! isUnity (0.5f));
            expect (! isUnity (-0.5f));

            // Those three pin nothing. 0.5 dB is five hundred times the real
            // tolerance, so widening isUnity from 0.001f to 0.4f passes all of
            // them unchanged -- and a wider tolerance means processBlock skips
            // the buffer on a trim the user can hear.
            //
            // The bottom of the band is already held: "the buffer is skipped on
            // the decibel rule, not a linear one" below feeds 0.0005 dB and
            // requires the skip to fire, so the tolerance cannot fall below
            // that without failing there. What nothing watched was everything
            // from 0.0005 up to 0.5. These four pin the threshold itself into
            // (0.0009, 0.0011] on both signs, which makes the 0.001 dB that
            // processBlock's comment promises to discard a checked number
            // instead of a claim.
            expect (isUnity (0.0009f),
                    "the tolerance was tightened below the 0.001 dB the skip is documented "
                    "to discard");
            expect (isUnity (-0.0009f),
                    "the tolerance is no longer symmetric about unity");
            expect (! isUnity (0.0011f),
                    "the tolerance was widened past 0.001 dB, so processBlock now skips the "
                    "buffer on a trim it is supposed to apply");
            expect (! isUnity (-0.0011f),
                    "the tolerance is no longer symmetric about unity");
        }

        beginTest ("the buffer is skipped on the decibel rule, not a linear one");
        {
            // isUnity is the rule processBlock asks, and it is a rule about
            // DECIBELS. 0.0005 dB is inside its tolerance while
            // decibelsToGain (0.0005f) is 1.0000576, which is not
            // approximatelyEqual to 1.0f -- so a linear test re-derived in
            // processBlock would multiply the buffer here and these samples
            // would not come back untouched. Exact equality is the point: it is
            // what distinguishes "skipped" from "multiplied by very nearly one".
            PreparedGain gain (0.0005f);
            const auto rendered = renderConstant (gain.processor, 0.5f, 2);

            float largestDeparture = 0.0f;

            for (const auto sample : rendered)
                largestDeparture = juce::jmax (largestDeparture, std::abs (sample - 0.5f));

            expectEquals (largestDeparture, 0.0f,
                          "the buffer was multiplied rather than skipped, so processBlock "
                          "is not asking isUnity");
        }
    }
};

static GainProcessorTests gainProcessorTests;
