#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Choosing a sample rate when the device will not accept the current one, as a
// decision rather than as control flow.
//
// WHY THIS IS A SEPARATE DECISION
//
// The host reacts to a device change by checking whether the current sample rate
// is one the device supports, and asking for a different one if it is not. That
// request makes the device manager broadcast another change, which brings us
// straight back here. The loop terminates only because the rate normally lands on
// something supported.
//
// A driver that reports a rate outside its own getAvailableSampleRates(), or that
// keeps rejecting the rate it just accepted, never terminates it. Up to 4.0.3 the
// only guard was a bool held across the callback by a ScopedValueSetter, which
// stops a *synchronous* re-entry. Change broadcasts are asynchronous
// (ChangeBroadcaster::sendChangeMessage posts an async update), so by the time the
// second callback arrives the flag is already clear and the guard has done
// nothing at all. The recursion it was written to stop is exactly the one it
// cannot see.
//
// What bounds it is counting attempts, and that is what this decides. The count
// resets as soon as the device reports a rate it actually supports, so a genuine
// device change later gets its full budget again; a driver that never settles is
// given up on, loudly, after a few tries.
//==============================================================================
namespace lighthost::samplerate
{
    /** Corrective attempts allowed before the driver is left alone.

        Three is enough for the legitimate cases (a device that needs one nudge,
        or two if the first choice is refused) and small enough that a driver
        which never settles stops being asked almost immediately.
    */
    static constexpr int kMaxCorrections = 3;

    /** Studio-standard rates, in the order they are preferred. */
    inline constexpr double kPreferredRates[] = { 48000.0, 44100.0, 96000.0, 88200.0, 192000.0, 32000.0 };

    enum class Action
    {
        keepCurrentRate,  ///< The rate is supported, or there is nothing to choose from
        applyRate,        ///< Ask the device for Decision::rate
        giveUp            ///< The driver will not settle; stop asking
    };

    struct Decision
    {
        Action action = Action::keepCurrentRate;

        /** Meaningful only when action is applyRate. */
        double rate = 0.0;

        /** True when the device reported a rate it supports, which ends the
            episode and restores the full budget for the next one.
        */
        bool resetAttempts = false;
    };

    /** Decides what to do about the current rate.

        `attemptsSoFar` is how many times we have already asked this device to
        change rate without it settling.
    */
    [[nodiscard]] inline Decision decide (double currentRate,
                                          const juce::Array<double>& availableRates,
                                          int attemptsSoFar)
    {
        // A device that reports no rates at all tells us nothing, and guessing
        // would be the start of the loop rather than the end of it.
        if (availableRates.isEmpty())
            return {};

        if (availableRates.contains (currentRate))
            return { Action::keepCurrentRate, 0.0, /*resetAttempts*/ true };

        if (attemptsSoFar >= kMaxCorrections)
            return { Action::giveUp, 0.0, false };

        for (const double rate : kPreferredRates)
            if (availableRates.contains (rate))
                return { Action::applyRate, rate, false };

        // Nothing standard on offer: take the highest the device admits to.
        return { Action::applyRate, availableRates.getLast(), false };
    }
}
