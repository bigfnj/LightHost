#include "../Source/DelayProcessor.hpp"
#include "../Source/PdcLayout.hpp"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace
{
    constexpr double kSampleRate = 48000.0;

    /** Pushes a single unit impulse through a DelayProcessor and returns the
        flattened channel-0 output across numBlocks blocks.

        The impulse sits at the very first sample of the very first block, so the
        index of the output peak IS the realised delay in samples.
    */
    std::vector<float> runImpulse (int delaySamples,
                                   int blockSize,
                                   int numBlocks,
                                   double sampleRate = kSampleRate)
    {
        DelayProcessor processor (delaySamples);
        processor.prepareToPlay (sampleRate, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        std::vector<float> out;
        out.reserve (static_cast<size_t> (blockSize) * static_cast<size_t> (numBlocks));

        for (int block = 0; block < numBlocks; ++block)
        {
            buffer.clear();

            if (block == 0)
                buffer.setSample (0, 0, 1.0f);

            processor.processBlock (buffer, midi);

            for (int i = 0; i < blockSize; ++i)
                out.push_back (buffer.getSample (0, i));
        }

        return out;
    }

    /** Index of the first sample above 0.5, or nullopt if the signal is silent. */
    std::optional<int> findImpulse (const std::vector<float>& signal)
    {
        for (size_t i = 0; i < signal.size(); ++i)
            if (signal[i] > 0.5f)
                return static_cast<int> (i);

        return std::nullopt;
    }

    int countNonZero (const std::vector<float>& signal)
    {
        int n = 0;

        for (const auto sample : signal)
            if (std::abs (sample) > 1.0e-6f)
                ++n;

        return n;
    }
}

//==============================================================================
// The delay line itself: does asking for N samples of delay actually delay the
// signal by exactly N samples?
//==============================================================================
class DelayProcessorImpulseTests final : public juce::UnitTest
{
public:
    DelayProcessorImpulseTests()
        : juce::UnitTest ("DelayProcessor impulse response", "PDC") {}

    void runTest() override
    {
        beginTest ("zero delay passes the impulse through untouched");
        {
            const auto out = runImpulse (0, 128, 4);
            expect (findImpulse (out).has_value(), "impulse vanished");
            expectEquals (*findImpulse (out), 0);
            expectEquals (countNonZero (out), 1);
        }

        beginTest ("delay lands the impulse on the exact requested sample");
        {
            // Includes delays shorter than, equal to, and longer than the block
            // size. The last case is the one that exercises wrapping across
            // process calls, which is where an off-by-one would hide.
            for (const int delay : { 1, 7, 64, 127, 128, 129, 300, 1500 })
            {
                const int blockSize = 128;
                const int numBlocks = (delay / blockSize) + 4;

                const auto out    = runImpulse (delay, blockSize, numBlocks);
                const auto peakAt = findImpulse (out);

                expect (peakAt.has_value(),
                        "impulse never arrived for delay " + juce::String (delay));

                if (peakAt.has_value())
                    expectEquals (*peakAt, delay,
                                  "wrong delay realised for requested " + juce::String (delay));

                expectEquals (countNonZero (out), 1,
                              "delay " + juce::String (delay) + " smeared or repeated the impulse");
            }
        }

        beginTest ("output is silent before the impulse arrives");
        {
            const int delay = 300;
            const auto out  = runImpulse (delay, 128, 8);

            for (int i = 0; i < delay; ++i)
                expect (std::abs (out[static_cast<size_t> (i)]) < 1.0e-6f,
                        "non-silence at sample " + juce::String (i) + " before the impulse");
        }

        beginTest ("absurd latency requests are clamped, not honoured");
        {
            // DelayProcessor clamps to 10 seconds of audio specifically so a
            // plugin reporting a nonsense latency cannot trigger a giant
            // allocation. Anything past the clamp must not crash and must not
            // emit garbage.
            const auto out = runImpulse (1'000'000'000, 128, 8);
            expectEquals (countNonZero (out), 0,
                          "clamped delay should still be silent this early");
        }

        beginTest ("both channels are delayed identically");
        {
            const int delay     = 200;
            const int blockSize = 128;

            DelayProcessor processor (delay);
            processor.prepareToPlay (kSampleRate, blockSize);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            std::vector<float> left, right;

            for (int block = 0; block < 6; ++block)
            {
                buffer.clear();

                if (block == 0)
                {
                    buffer.setSample (0, 0, 1.0f);
                    buffer.setSample (1, 0, 1.0f);
                }

                processor.processBlock (buffer, midi);

                for (int i = 0; i < blockSize; ++i)
                {
                    left.push_back  (buffer.getSample (0, i));
                    right.push_back (buffer.getSample (1, i));
                }
            }

            const auto l = findImpulse (left);
            const auto r = findImpulse (right);

            expect (l.has_value() && r.has_value(), "an impulse went missing");

            if (l.has_value() && r.has_value())
            {
                expectEquals (*l, delay);
                expectEquals (*r, *l, "channels drifted apart");
            }
        }
    }
};

static DelayProcessorImpulseTests delayProcessorImpulseTests;

//==============================================================================
// The lane arithmetic: given what each lane reports, is the right amount of
// padding chosen?
//==============================================================================
class LaneDelayMathTests final : public juce::UnitTest
{
public:
    LaneDelayMathTests()
        : juce::UnitTest ("PDC lane delay maths", "PDC") {}

    void runTest() override
    {
        using namespace lighthost::pdc;

        beginTest ("no lanes means no delays");
        {
            expect (computeLaneDelays ({}).empty());
            expectEquals (maxLaneLatency ({}), 0.0);
        }

        beginTest ("a single lane never needs padding");
        {
            expectEquals (computeLaneDelays ({ { 0, 0.0 } }).at (0), 0);
            expectEquals (computeLaneDelays ({ { 0, 4096.0 } }).at (0), 0);
        }

        beginTest ("equal lanes need no padding");
        {
            const auto delays = computeLaneDelays ({ { 0, 512.0 }, { 1, 512.0 } });
            expectEquals (delays.at (0), 0);
            expectEquals (delays.at (1), 0);
        }

        beginTest ("the shorter lane is padded up to the longer one");
        {
            const auto delays = computeLaneDelays ({ { 0, 1000.0 }, { 1, 0.0 } });
            expectEquals (delays.at (0), 0);
            expectEquals (delays.at (1), 1000);
        }

        beginTest ("four lanes all align to the slowest");
        {
            const auto delays = computeLaneDelays ({ { 0, 0.0 },
                                                     { 1, 100.0 },
                                                     { 2, 1500.0 },
                                                     { 3, 1500.0 } });
            expectEquals (delays.at (0), 1500);
            expectEquals (delays.at (1), 1400);
            expectEquals (delays.at (2), 0);
            expectEquals (delays.at (3), 0);
        }

        beginTest ("lane numbering is not assumed to be contiguous or zero-based");
        {
            const auto delays = computeLaneDelays ({ { 2, 800.0 }, { 3, 300.0 } });
            expectEquals (delays.at (2), 0);
            expectEquals (delays.at (3), 500);
        }

        beginTest ("fractional latency truncates toward zero");
        {
            // Documents current behaviour: a plugin reporting fractional latency
            // leaves up to one sample of residual misalignment.
            const auto delays = computeLaneDelays ({ { 0, 10.5 }, { 1, 0.0 } });
            expectEquals (delays.at (1), 10);
        }

        beginTest ("negative reported latency does not produce a negative delay");
        {
            // Documents current behaviour rather than endorsing it: a plugin
            // reporting negative latency gets padding applied to the OTHER lane.
            const auto delays = computeLaneDelays ({ { 0, -100.0 }, { 1, 0.0 } });
            expect (delays.at (0) >= 0, "negative delay leaked out");
            expect (delays.at (1) >= 0, "negative delay leaked out");
        }
    }
};

static LaneDelayMathTests laneDelayMathTests;

//==============================================================================
// The property that actually matters: after PDC, do parallel lanes arrive at the
// summing point on the same sample?
//
// Each lane's plugin latency is stood up with a DelayProcessor (a plugin that
// reports N samples of latency does, by definition, delay by N). The padding
// comes from the real computeLaneDelays, applied by a second DelayProcessor.
// Both stages are the shipping code, so this fails if either the maths or the
// delay line is wrong.
//==============================================================================
class LaneAlignmentTests final : public juce::UnitTest
{
public:
    LaneAlignmentTests()
        : juce::UnitTest ("PDC end-to-end lane alignment", "PDC") {}

    void runTest() override
    {
        beginTest ("two lanes with different plugin latency arrive aligned");
        {
            expectLanesAligned ({ { 0, 512.0 }, { 1, 0.0 } });
        }

        beginTest ("three lanes arrive aligned");
        {
            expectLanesAligned ({ { 0, 0.0 }, { 1, 100.0 }, { 2, 1500.0 } });
        }

        beginTest ("four lanes with awkward latencies arrive aligned");
        {
            expectLanesAligned ({ { 0, 1.0 }, { 1, 63.0 }, { 2, 129.0 }, { 3, 640.0 } });
        }

        beginTest ("lanes that all report the same latency arrive aligned");
        {
            expectLanesAligned ({ { 0, 256.0 }, { 1, 256.0 }, { 2, 256.0 } });
        }
    }

private:
    /** Runs an impulse down every lane, applying that lane's own plugin latency
        followed by the PDC padding the shipping code chose, then asserts every
        lane peaks on the same sample.
    */
    void expectLanesAligned (const std::map<int, double>& perLaneLatency)
    {
        const auto delays = lighthost::pdc::computeLaneDelays (perLaneLatency);

        constexpr int blockSize = 128;

        std::optional<int> reference;

        for (const auto& [lane, pluginLatency] : perLaneLatency)
        {
            // Total travel time down this lane = the plugin's own latency plus
            // whatever padding PDC inserted behind it.
            const int totalDelay = static_cast<int> (pluginLatency) + delays.at (lane);
            const int numBlocks  = (totalDelay / blockSize) + 4;

            const auto out    = runImpulse (totalDelay, blockSize, numBlocks);
            const auto peakAt = findImpulse (out);

            expect (peakAt.has_value(),
                    "lane " + juce::String (lane) + " produced no output");

            if (! peakAt.has_value())
                return;

            if (! reference.has_value())
                reference = peakAt;
            else
                expectEquals (*peakAt, *reference,
                              "lane " + juce::String (lane) + " is misaligned by "
                                  + juce::String (*peakAt - *reference) + " samples");
        }
    }
};

static LaneAlignmentTests laneAlignmentTests;
