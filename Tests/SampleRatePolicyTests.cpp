#include "../Source/SampleRatePolicy.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// The sample-rate correction decision, including the bound that stops a driver
// which never settles from being asked forever.
//==============================================================================
namespace
{
    using namespace lighthost::samplerate;

    juce::Array<double> rates (std::initializer_list<double> values)
    {
        juce::Array<double> array;

        for (const double value : values)
            array.add (value);

        return array;
    }
}

//==============================================================================
class SampleRatePolicyTests final : public juce::UnitTest
{
public:
    SampleRatePolicyTests()
        : juce::UnitTest ("Sample rate correction", "SampleRate") {}

    void runTest() override
    {
        beginTest ("a supported rate is left alone, and ends the episode");
        {
            const auto decision = decide (48000.0, rates ({ 44100.0, 48000.0, 96000.0 }), 0);

            expect (decision.action == Action::keepCurrentRate);
            expect (decision.resetAttempts, "a settled device should restore the budget");
        }

        beginTest ("an unsupported rate is corrected to the preferred one");
        {
            const auto decision = decide (192000.0, rates ({ 44100.0, 48000.0 }), 0);

            expect (decision.action == Action::applyRate);
            expectEquals (decision.rate, 48000.0);
        }

        beginTest ("the preference order is honoured when 48k is missing");
        {
            const auto decision = decide (192000.0, rates ({ 44100.0, 96000.0 }), 0);

            expect (decision.action == Action::applyRate);
            expectEquals (decision.rate, 44100.0);
        }

        beginTest ("a device offering nothing standard gets its highest rate");
        {
            const auto decision = decide (48000.0, rates ({ 8000.0, 11025.0, 22050.0 }), 0);

            expect (decision.action == Action::applyRate);
            expectEquals (decision.rate, 22050.0);
        }

        beginTest ("a device that reports no rates at all is not guessed at");
        {
            // Asking a device that admits to no rates would be the start of the
            // loop, not the end of it.
            const auto decision = decide (48000.0, rates ({}), 0);

            expect (decision.action == Action::keepCurrentRate);
            expect (! decision.resetAttempts);
        }

        beginTest ("a driver that never settles is given up on");
        {
            // This is the bound. Up to 4.0.3 nothing stopped this: the guard was a
            // bool held across one callback, and change broadcasts are async, so
            // the second callback always found it clear.
            const auto unsupported = rates ({ 44100.0, 48000.0 });

            for (int attempt = 0; attempt < kMaxCorrections; ++attempt)
                expect (decide (192000.0, unsupported, attempt).action == Action::applyRate,
                        "attempt " + juce::String (attempt) + " should still try");

            expect (decide (192000.0, unsupported, kMaxCorrections).action == Action::giveUp);
            expect (decide (192000.0, unsupported, kMaxCorrections + 40).action == Action::giveUp);
        }

        beginTest ("giving up on one device does not disarm the next");
        {
            // The counter resets the moment a device reports a rate it supports,
            // so a later genuine change gets the full budget again.
            const auto settled = decide (48000.0, rates ({ 48000.0 }), kMaxCorrections);

            expect (settled.action == Action::keepCurrentRate);
            expect (settled.resetAttempts, "the budget should be restored once settled");
            expect (decide (192000.0, rates ({ 48000.0 }), 0).action == Action::applyRate);
        }
    }
};

static SampleRatePolicyTests sampleRatePolicyTests;
