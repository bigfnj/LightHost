#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>

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
// What bounds it is counting attempts, and that is what this decides. A driver
// that never settles is given up on, loudly, after a few tries.
//
// WHERE THE COUNT IS RESET, AND WHY IT IS NOT ALL HERE
//
// Two things end an episode, and only one of them is visible from inside a
// decision:
//
//   the device settles   decide() sees it, because the rate it is handed is one
//                        the device admits to, and says so via resetAttempts.
//
//   the device changes   decide() cannot see it. It is handed a rate and a list
//                        of rates, never an identity, and the reset above can
//                        never fire for a device already given up on: the rate
//                        it is stuck at is by definition one that device says it
//                        does not support.
//
// So the caller owns the second one. IconMenu holds a Budget, names its device
// on every callback, and the Budget zeroes the count when that name moves. Up
// to 5.2.0 there was no such reset, and this header claimed "a genuine device
// change later gets its full budget again" while device A exhausting its budget
// meant device B was given up on by its first callback, having been asked for
// nothing. The identity is still not a parameter of decide(): that stays a pure
// function of a rate and a list, so the budget can be exercised without a
// device.
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
        //
        // std::max_element, not getLast(). "The highest" and "the last" are the
        // same value only while the list is sorted ascending, and nothing
        // promises that: juce::AudioIODevice::getAvailableSampleRates() is
        // documented as returning "the SET of sample-rates this device
        // supports", and a set has no order. Every backend in this build
        // happens to return them ascending, so a driver that did not would
        // have us hand the device its LOWEST rate while the line above claimed
        // the opposite -- 8000 out of { 96000, 8000 }, on a device offering 96k.
        //
        // Narrow on purpose: reaching this line needs a device offering none of
        // the six preferred rates yet offering something, which no device seen
        // here does. It is a linear scan of a list a dozen entries long, once
        // per device change, so being right about it costs nothing measurable.
        // The empty case returned at the top of this function, so there is
        // always an element to dereference.
        return { Action::applyRate,
                 *std::max_element (availableRates.begin(), availableRates.end()),
                 false };
    }

    //==========================================================================
    /** The attempt count decide() is handed, and the two things that end an
        episode.

        Held by the caller across callbacks, because decide() is deliberately a
        pure function of a rate and a list. It lives here rather than as loose
        members of IconMenu so the DEVICE-CHANGE reset is reachable from a test:
        it was one loose int there, with no device half at all, and that is how
        the header comment above came to describe a reset that did not happen.
        A rule that only exists inside a component no test can construct is a
        rule nothing checks.
    */
    class Budget
    {
    public:
        /** What to hand decide() as `attemptsSoFar`. */
        [[nodiscard]] int attempts() const noexcept { return attemptsMade; }

        /** Names the device the next decision is about, before making it.

            A different identity is a different episode: full budget, and a
            give-up worth logging again. The identity is whatever tells devices
            apart; IconMenu uses driver type + device name. Returns true when
            the budget was restored, which is a fact worth logging and worth
            asserting.

            Deliberately NOT reset when a device goes away, only when a
            different one arrives. Treating every momentary close as a new
            episode would hand three more requests to each one, which is the
            unbounded loop this whole file exists to stop.
        */
        bool useDevice (const juce::String& identity)
        {
            if (identity == deviceIdentity)
                return false;

            deviceIdentity = identity;
            restore();
            return true;
        }

        /** Records what decide() returned, before acting on it.

            A settle restores the budget; a request spends one. giveUp spends
            nothing: the count is already at the limit there, so counting it
            again only makes an unbounded number out of a bounded one.
        */
        void note (const Decision& decision)
        {
            if (decision.resetAttempts)
                restore();
            else if (decision.action == Action::applyRate)
                ++attemptsMade;
        }

        /** True the first time a give-up is reached in an episode, false after.

            Every decision after a give-up is another give-up -- the count stays
            at the limit until something restores it -- so a log guarded only by
            reaching the branch repeats on every device broadcast for as long as
            the driver misbehaves.
        */
        [[nodiscard]] bool takeGiveUpLog() noexcept
        {
            if (giveUpLogged)
                return false;

            giveUpLogged = true;
            return true;
        }

    private:
        void restore() noexcept
        {
            attemptsMade = 0;
            giveUpLogged = false;
        }

        juce::String deviceIdentity;
        int  attemptsMade = 0;
        bool giveUpLogged = false;
    };
}
