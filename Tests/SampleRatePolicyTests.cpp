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

        beginTest ("a settled device restores the budget from inside the decision");
        {
            // The only reset decide() can see for itself.
            const auto settled = decide (48000.0, rates ({ 48000.0 }), kMaxCorrections);

            expect (settled.action == Action::keepCurrentRate);
            expect (settled.resetAttempts, "the budget should be restored once settled");
        }

        beginTest ("a spent budget gives up whatever device spent it");
        {
            // The call the test named "giving up on one device does not disarm
            // the next" was missing. It only ever asked
            // decide (48000.0, rates ({ 48000.0 }), kMaxCorrections), which
            // returns at the supported-rate check above before the budget check
            // is reached -- so it passed no matter what the budget did, and the
            // defect it was named after was invisible to it.
            //
            // This is what that check actually says: decide() has no idea which
            // device spent the count, so a new device whose rates exclude the
            // current rate is given up on by its first callback.
            expect (decide (192000.0, rates ({ 48000.0 }), kMaxCorrections).action
                        == Action::giveUp,
                    "a spent count gives up regardless of which device spent it");

            // Which is why the device half of the reset has to come from
            // outside. Given it, the same device and rates correct normally.
            expect (decide (192000.0, rates ({ 48000.0 }), 0).action == Action::applyRate);
        }

        beginTest ("the budget spends one attempt per request, and nothing else");
        {
            Budget budget;
            budget.useDevice ("wasapi/Interface A");

            expectEquals (budget.attempts(), 0);

            budget.note ({ Action::applyRate, 48000.0, false });
            expectEquals (budget.attempts(), 1);

            budget.note ({ Action::keepCurrentRate, 0.0, false });
            expectEquals (budget.attempts(), 1, "nothing was asked for, so nothing was spent");

            budget.note ({ Action::giveUp, 0.0, false });
            expectEquals (budget.attempts(), 1,
                          "giving up counted as an attempt, which makes a bounded count unbounded");
        }

        beginTest ("settling restores the budget the caller holds");
        {
            Budget budget;
            budget.useDevice ("wasapi/Interface A");

            for (int i = 0; i < kMaxCorrections; ++i)
                budget.note ({ Action::applyRate, 48000.0, false });

            expectEquals (budget.attempts(), kMaxCorrections);

            budget.note ({ Action::keepCurrentRate, 0.0, /*resetAttempts*/ true });
            expectEquals (budget.attempts(), 0);
        }

        beginTest ("giving up on one device does not disarm the next");
        {
            // The property the old test named and could not reach. Spend device
            // A's budget through the real decision, then arrive at device B
            // whose rates also exclude the current rate: B must be corrected,
            // not given up on before it has been asked for anything.
            Budget budget;
            budget.useDevice ("wasapi/Interface A");

            for (int i = 0; i < kMaxCorrections; ++i)
                budget.note (decide (192000.0, rates ({ 48000.0 }), budget.attempts()));

            expect (decide (192000.0, rates ({ 48000.0 }), budget.attempts()).action
                        == Action::giveUp,
                    "device A should be out of budget by now");

            expect (budget.useDevice ("wasapi/Interface B"),
                    "a different device should be reported as a new episode");

            expect (decide (192000.0, rates ({ 48000.0 }), budget.attempts()).action
                        == Action::applyRate,
                    "device B was given up on before it had been asked for anything");
        }

        beginTest ("the same device is not a new episode");
        {
            // Sticky on purpose. Treating a repeat callback, or a device that
            // closes and reopens under one name, as a fresh episode would hand
            // three more requests to every one of them, which is the unbounded
            // loop the count exists to stop.
            Budget budget;
            expect (budget.useDevice ("wasapi/Interface A"), "the first device is a change");
            expect (! budget.useDevice ("wasapi/Interface A"));

            for (int i = 0; i < kMaxCorrections; ++i)
                budget.note ({ Action::applyRate, 48000.0, false });

            expect (! budget.useDevice ("wasapi/Interface A"));
            expectEquals (budget.attempts(), kMaxCorrections,
                          "the same device was handed its budget back");
        }

        beginTest ("the give-up is logged once per episode, not once per broadcast");
        {
            Budget budget;
            budget.useDevice ("wasapi/Interface A");

            expect (budget.takeGiveUpLog(), "the first give-up in an episode is worth saying");
            expect (! budget.takeGiveUpLog(),
                    "the message repeats on every later broadcast, which is every device change "
                    "for as long as the driver misbehaves");
            expect (! budget.takeGiveUpLog());

            expect (budget.useDevice ("wasapi/Interface B"),
                    "a different device is a new episode and worth saying again");
            expect (budget.takeGiveUpLog());
            expect (! budget.takeGiveUpLog());

            budget.note ({ Action::keepCurrentRate, 0.0, /*resetAttempts*/ true });
            expect (budget.takeGiveUpLog(),
                    "a device that settled and then went wrong again is a second episode");
        }
    }
};

static SampleRatePolicyTests sampleRatePolicyTests;
