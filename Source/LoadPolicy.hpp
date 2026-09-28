#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <optional>

//==============================================================================
// How close the audio thread is to missing its deadline, and when to say so.
//
// WHY THIS EXISTS
//
// Light Host had no way to tell a user -- or a developer -- whether audio was
// actually dropping out. "It gets bad when the machine is busy" was the whole of
// the evidence, and every hypothesis after that was unfalsifiable: a plugin
// eating CPU, the host wasting it, or Windows scheduling the audio thread onto
// an efficiency core all present identically when nothing is measured.
//
// WHY THERE IS NO AUDIO-THREAD CODE HERE
//
// The measurement is already being taken. AudioDeviceManager wraps EVERY
// registered callback in an AudioProcessLoadMeasurer::ScopedTimer
// (juce_AudioDeviceManager.cpp:1084), so getCpuUsage() and getXRunCount()
// already describe our DeviceTap and everything under it. This file is reached
// from a message-thread poll and touches no audio buffer at all.
//
// One caveat worth knowing rather than fixing: AudioProcessLoadMeasurer's
// registerRenderTime takes a try-lock and drops the timing for that block when
// it fails. The only contention is reset() on the message thread, so a dropout
// at the exact instant of a device change can be lost. Rare, and not worth an
// audio-thread change to recover.
//
// THE -1 THAT HAS TO BE UNDONE HERE
//
// AudioDeviceManager::getXRunCount() returns jmax(0, deviceXRuns) plus its own
// measured count. WASAPI reports -1 when there is no input device
// (juce_WASAPI_windows.cpp:1369), so that clamp makes "this driver does not
// report under-runs" indistinguishable from "this driver reports zero". Those
// are different facts and the user is owed the difference: a clean run and an
// unmonitored one must not read the same. Given the device number as well, the
// split is exact, so the caller reads both and splitTotals undoes the clamp.
//
// The two halves also mean different things. A driver-reported xrun is an
// audible glitch the hardware noticed. A measured one only says the callback
// took longer than its block, which at a 2.7 ms buffer a single scheduling
// hiccup produces and the driver may have absorbed inaudibly. So they are
// counted apart, displayed apart, and escalated on different thresholds.
//
// WHY THE ESCALATION IS BUDGETED
//
// status::Sink remembers 8 problems and has no dismiss. A machine dropping
// audio continuously would push a fresh message every poll, evicting every
// other problem the user had -- so the feature reporting the trouble would
// destroy the evidence of everything else. Two reports per episode is "it
// started" and "it is still going, and this is the last you will hear of it".
//==============================================================================
namespace lighthost::load
{
    /** Load at which the readout stops being reassuring, as a proportion of the
        block deadline.

        Not a dropout threshold -- it is a headroom one. Below 70% a burst of
        work has somewhere to go; above it, the next plugin that decides to
        think will not fit.
    */
    inline constexpr double kCautionProportion = 0.70;

    /** Load at which a dropout is a matter of time rather than of luck. */
    inline constexpr double kHotProportion = 0.90;

    /** How often the message thread folds a reading in.

        Dropout counters are cumulative, so no poll rate can lose one; only the
        peak hold under-samples. A load spike that decays inside half a second
        and caused no xrun is not an incident.
    */
    inline constexpr int kPollMs = 500;

    /** How recently a dropout has to have happened for the readout to be hot
        rather than merely cautionary. */
    inline constexpr juce::int64 kRecentDropoutMs = 5000;

    /** Quiet time that ends an episode. A minute without a dropout means the
        next one is news again. */
    inline constexpr juce::int64 kEpisodeQuietMs = 60000;

    /** Minimum spacing between two reports inside one episode. */
    inline constexpr juce::int64 kRepeatMs = 300000;

    /** See the banner. Two reports is an eighth of the sink; the rest of it
        stays available for everything else that can go wrong. */
    inline constexpr int kMaxReportsPerEpisode = 2;

    /** Measured-only dropouts needed before the user is told.

        A driver-reported xrun escalates on the first one, because the hardware
        noticed it and that means it was audible. A measured one is "the
        callback overran its block", which one scheduling hiccup produces --
        reporting that as "audio is dropping out" is a false alarm, and false
        alarms teach people to ignore the status row.
    */
    inline constexpr int kMeasuredEscalationMin = 5;

    //==========================================================================
    enum class Severity { ok, caution, hot };

    /** The two dropout counters, kept apart because they mean different things.

        See the banner: driver-reported means the hardware noticed, measured
        means only that a block overran.
    */
    struct Counts
    {
        int driver   = 0;
        int measured = 0;

        [[nodiscard]] int total() const noexcept { return driver + measured; }
    };

    /** One message-thread reading.

        Time is a member rather than something this file takes from the clock,
        so an hour of a misbehaving machine can be driven through a Monitor in a
        test with no device and no waiting.
    */
    struct Sample
    {
        bool         deviceOpen      = false;
        double       loadProportion  = 0.0;  ///< AudioDeviceManager::getCpuUsage()
        int          xrunTotal       = 0;    ///< AudioDeviceManager::getXRunCount()
        int          deviceXrunTotal = -1;   ///< the device's own, -1 = unsupported
        double       blockMs         = 0.0;  ///< the deadline a callback has to meet
        juce::String coreLabel;              ///< "P" / "E" / empty; see ProcessFacts.hpp
        juce::int64  atMs            = 0;    ///< juce::Time::getMillisecondCounter()
    };

    struct Readout
    {
        juce::String text;
        Severity     severity = Severity::ok;
    };

    struct Escalation
    {
        bool         report = false;
        juce::String message;
    };

    //==========================================================================
    /** True when the device reports its own under-runs at all.

        A named predicate rather than an inline >= 0, because the whole point is
        that the caller must never treat -1 as a count.
    */
    [[nodiscard]] inline bool driverReportsDropouts (int deviceTotal) noexcept
    {
        return deviceTotal >= 0;
    }

    /** Splits the manager total back into its two sources.

        See the banner for why the manager accessor cannot be used alone. A
        negative device total contributes nothing, and the remainder is never
        allowed below zero: a driver whose counter restarted since the manager
        last folded it in would otherwise produce a negative measured count, and
        every delta taken off it afterwards would be wrong.
    */
    [[nodiscard]] inline Counts splitTotals (int managerTotal, int deviceTotal) noexcept
    {
        const auto driver = driverReportsDropouts (deviceTotal) ? deviceTotal : 0;
        return { driver, std::max (0, managerTotal - driver) };
    }

    /** New dropouts since the last reading, with a counter restart treated as a
        restart rather than as a negative delta.

        JUCE resets its load measurer on device start and stop
        (juce_AudioDeviceManager.cpp:961, :1142, :1163), and the WASAPI counter
        restarts when the device opens, so this number CAN go down. Subtracting
        straight through would yield a large negative, and the running total it
        feeds would then have to climb back past its old value before a single
        further dropout became visible.
    */
    [[nodiscard]] inline int deltaFrom (int previousTotal, int currentTotal) noexcept
    {
        return currentTotal < previousTotal ? currentTotal
                                            : currentTotal - previousTotal;
    }

    /** How alarmed to look.

        Dropouts outrank load, because a dropout IS the failure and load is only
        a predictor of one. A machine sitting at 5% that glitched ten seconds ago
        has a problem; one sitting at 95% that has never glitched does not, yet.
    */
    [[nodiscard]] inline Severity severityFor (double loadProportion,
                                               int dropoutsSoFar,
                                               std::optional<juce::int64> sinceLastDropoutMs) noexcept
    {
        if (dropoutsSoFar > 0)
        {
            if (sinceLastDropoutMs.has_value() && *sinceLastDropoutMs <= kRecentDropoutMs)
                return Severity::hot;

            return Severity::caution;
        }

        if (loadProportion >= kHotProportion)     return Severity::hot;
        if (loadProportion >= kCautionProportion) return Severity::caution;

        return Severity::ok;
    }

    [[nodiscard]] inline juce::String asPercent (double proportion)
    {
        return juce::String (juce::roundToInt (proportion * 100.0)) + "%";
    }

    [[nodiscard]] inline juce::String describeLoad (double now, double peak)
    {
        return asPercent (now) + " (peak " + asPercent (peak) + ")";
    }

    /** The dropout half of the readout.

        Five shapes, and the one that matters is the second: a driver that does
        not report under-runs must not read the same as one reporting none, or
        an unmonitored run looks like a clean one.
    */
    [[nodiscard]] inline juce::String describeDropouts (Counts counts, bool driverReports)
    {
        if (counts.total() == 0)
            return driverReports
                     ? juce::String ("no dropouts")
                     : juce::String ("no dropouts (measured; this driver reports none of its own)");

        const auto headline = juce::String (counts.total())
                            + (counts.total() == 1 ? " dropout" : " dropouts");

        if (! driverReports)
            return headline + " (measured, not reported by the driver)";

        if (counts.measured == 0)
            return headline;

        return headline + " (" + juce::String (counts.driver) + " from the driver)";
    }

    /** Joins the parts, omitting any that is empty.

        The core label is absent on every platform but Windows, and absent there
        until the audio thread has been sampled, so the separator cannot be
        baked into the caller.
    */
    [[nodiscard]] inline juce::String composeReadout (const juce::String& load,
                                                      const juce::String& dropouts,
                                                      const juce::String& coreLabel)
    {
        juce::StringArray parts;

        if (load.isNotEmpty())      parts.add (load);
        if (dropouts.isNotEmpty())  parts.add (dropouts);
        if (coreLabel.isNotEmpty()) parts.add ("core " + coreLabel);

        return parts.joinIntoString ("  -  ");
    }

    //==========================================================================
    /** The running state behind the readout: peak hold, session counts, and the
        report budget.

        Held by the caller across polls, the way samplerate::Budget is, and for
        the same reason -- the decisions above stay pure functions so they can be
        exercised without a device, and the state that spans callbacks lives in a
        type a test can drive.
    */
    class Monitor
    {
    public:
        /** Names the device session the next observation belongs to.

            A different identity restarts the peak, both counts and the report
            budget; returns true when it did, which is worth logging and worth
            asserting.

            The identity must include the sample rate and the buffer size, not
            just the device name. The load proportion is defined against the
            block deadline, so a peak of 88% at 480 samples and one at 128
            samples are not the same measurement and must not share a hold --
            and JUCE resets the counter behind them on any device start anyway,
            so a narrower key would display a peak whose source had already
            discarded it.

            Deliberately NOT reset when a device merely goes away. Devices close
            for a moment during a change, and treating that as a new session
            would throw away the peak that explains what just happened.
        */
        bool useSession (const juce::String& identity)
        {
            if (identity == sessionIdentity)
                return false;

            sessionIdentity   = identity;
            session           = {};
            episode           = {};
            lastSeen          = {};
            peak              = 0.0;
            current           = 0.0;
            deadlineMs        = 0.0;
            driverReports     = false;
            haveSeenSample    = false;
            reportsMade       = 0;
            totalAtLastReport = 0;
            lastDropoutMs.reset();
            lastReportMs.reset();
            cached = {};
            return true;
        }

        /** Folds one reading in, and says whether the user should be told. */
        [[nodiscard]] Escalation observe (const Sample& sample)
        {
            if (! sample.deviceOpen)
            {
                cached = { "no audio device", Severity::ok };
                return {};
            }

            const auto now = splitTotals (sample.xrunTotal, sample.deviceXrunTotal);

            // Split first, then delta each half, so a driver counter restarting
            // cannot forge a measured delta out of the manager total.
            //
            // The first sample of a session counts in full rather than being
            // adopted as a baseline: a device that dropped audio while it was
            // opening is exactly the case worth seeing, and adopting would hide
            // the whole burst.
            const auto driverDelta   = haveSeenSample ? deltaFrom (lastSeen.driver,   now.driver)
                                                      : now.driver;
            const auto measuredDelta = haveSeenSample ? deltaFrom (lastSeen.measured, now.measured)
                                                      : now.measured;

            lastSeen       = now;
            haveSeenSample = true;

            if (driverDelta + measuredDelta > 0)
            {
                // A quiet minute makes this a new episode, with a fresh budget.
                if (lastDropoutMs.has_value() && sample.atMs - *lastDropoutMs >= kEpisodeQuietMs)
                {
                    episode     = {};
                    reportsMade = 0;
                    lastReportMs.reset();
                }

                lastDropoutMs = sample.atMs;
            }

            session.driver   += driverDelta;
            session.measured += measuredDelta;
            episode.driver   += driverDelta;
            episode.measured += measuredDelta;

            current       = sample.loadProportion;
            peak          = std::max (peak, sample.loadProportion);
            deadlineMs    = sample.blockMs;
            driverReports = driverReportsDropouts (sample.deviceXrunTotal);

            const auto since = lastDropoutMs.has_value()
                                 ? std::optional<juce::int64> (sample.atMs - *lastDropoutMs)
                                 : std::nullopt;

            cached = { composeReadout (describeLoad (current, peak),
                                       describeDropouts (session, driverReports),
                                       sample.coreLabel),
                       severityFor (current, session.total(), since) };

            return decideEscalation (sample);
        }

        /** What the UI draws. Computed in observe() and cached, so the timer
            that paints does no work and needs no clock of its own. */
        [[nodiscard]] Readout readout() const { return cached; }

        /** The k=v tail of an AudioLoad log line. */
        [[nodiscard]] juce::String logFields() const
        {
            if (! haveSeenSample)
                return "device=none";

            return "load=" + asPercent (current)
                 + " peak=" + asPercent (peak)
                 + " dropouts=" + juce::String (session.total())
                 + " driver=" + juce::String (session.driver)
                 + " measured=" + juce::String (session.measured)
                 + " driverXruns=" + juce::String (driverReports ? "yes" : "no")
                 + " deadline=" + juce::String (deadlineMs, 1) + "ms";
        }

        [[nodiscard]] double peakProportion()     const noexcept { return peak; }
        [[nodiscard]] Counts dropouts()           const noexcept { return session; }
        [[nodiscard]] int    reportsThisEpisode() const noexcept { return reportsMade; }

    private:
        [[nodiscard]] Escalation decideEscalation (const Sample& sample)
        {
            const auto worthReporting = episode.driver > 0
                                     || episode.measured >= kMeasuredEscalationMin;

            if (! worthReporting || reportsMade >= kMaxReportsPerEpisode)
                return {};

            if (lastReportMs.has_value())
            {
                const auto spacedEnough = sample.atMs - *lastReportMs >= kRepeatMs;
                const auto somethingNew = session.total() > totalAtLastReport;

                // Both, not either: spacing alone repeats a stale message every
                // five minutes on a machine that has stopped dropping out.
                if (! (spacedEnough && somethingNew))
                    return {};
            }

            ++reportsMade;
            lastReportMs      = sample.atMs;
            totalAtLastReport = session.total();

            return { true, reportsMade >= kMaxReportsPerEpisode ? lastMessage()
                                                                : firstMessage() };
        }

        [[nodiscard]] juce::String firstMessage() const
        {
            auto message = "Audio is dropping out: " + describeDropouts (session, driverReports)
                         + " since this device was opened.";

            if (deadlineMs > 0.0)
                message += " The chain is not finishing inside the "
                         + juce::String (deadlineMs, 1) + " ms buffer.";

            return message + " Raise Buffer Size in Preferences, or bypass a plugin.";
        }

        [[nodiscard]] juce::String lastMessage() const
        {
            return "Audio is still dropping out (" + juce::String (session.total())
                 + " so far). This is the last report for this device; the log keeps counting.";
        }

        juce::String sessionIdentity;
        Counts session;
        Counts episode;
        Counts lastSeen;
        double peak              = 0.0;
        double current           = 0.0;
        double deadlineMs        = 0.0;
        bool   driverReports     = false;
        bool   haveSeenSample    = false;
        int    reportsMade       = 0;
        int    totalAtLastReport = 0;
        std::optional<juce::int64> lastDropoutMs;
        std::optional<juce::int64> lastReportMs;
        Readout cached;
    };
}
