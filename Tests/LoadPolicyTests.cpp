#include "../Source/LoadPolicy.hpp"
#include "../Source/StatusSink.hpp"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

//==============================================================================
// Audio load and dropout reporting.
//
// Two properties here are load-bearing and neither is obvious from the code.
//
// The first is that a driver reporting NO under-run count must not read the
// same as one reporting zero of them. AudioDeviceManager collapses the two with
// a jmax(0, ...), and an unmonitored run that looks like a clean run is worse
// than no feature at all.
//
// The second is the report budget. status::Sink holds eight problems and cannot
// be dismissed, so a machine dropping audio all afternoon must not be able to
// evict everything else the user was told. That one is asserted end to end,
// against a real Sink, because it is a claim about two files agreeing.
//==============================================================================
namespace
{
    using namespace lighthost::load;

    /** One reading, spelled out at each call site so the cases stay short. */
    [[nodiscard]] Sample sampleAt (juce::int64 atMs,
                                   double load,
                                   int managerTotal,
                                   int deviceTotal)
    {
        Sample sample;
        sample.deviceOpen      = true;
        sample.loadProportion  = load;
        sample.xrunTotal       = managerTotal;
        sample.deviceXrunTotal = deviceTotal;
        sample.blockMs         = 2.7;
        sample.atMs            = atMs;
        return sample;
    }

    //==========================================================================
    class LoadPolicyTests final : public juce::UnitTest
    {
    public:
        LoadPolicyTests()
            : juce::UnitTest ("Audio load and dropouts", "AudioLoad") {}

        void runTest() override
        {
            beginTest ("the manager total splits back into driver and measured");
            {
                // FAILS IF: the -1 is taken as a count, or the measured half is
                // computed as the whole. AudioDeviceManager::getXRunCount adds
                // jmax(0, deviceXRuns) to its own measured count, so the device
                // number has to be subtracted back out to recover either.
                expectEquals (splitTotals (7, 3).driver, 3);
                expectEquals (splitTotals (7, 3).measured, 4);
                expectEquals (splitTotals (7, -1).driver, 0);
                expectEquals (splitTotals (7, -1).measured, 7);
                expectEquals (splitTotals (0, -1).total(), 0);
            }

            beginTest ("a driver counter that restarted cannot make measured negative");
            {
                // The manager folded in an old, larger device count; the device
                // has since restarted. The remainder must floor at zero, or
                // every delta taken off it afterwards is wrong.
                expectEquals (splitTotals (2, 40).measured, 0);
            }

            beginTest ("a driver that does not report under-runs is not shown as a clean run");
            {
                const auto silent   = describeDropouts ({ 0, 0 }, false);
                const auto reported = describeDropouts ({ 0, 0 }, true);

                expect (silent != reported,
                        "a driver reporting nothing reads identically to one reporting zero, "
                        "so an unmonitored run looks like a clean one");
                expect (silent.contains ("driver"),
                        "the text does not say which driver behaviour produced it");
            }

            beginTest ("a restarted counter is a restart, not a negative delta");
            {
                // FAILS IF: JUCE resetting its measurer on device start silently
                // hides every dropout that follows.
                expectEquals (deltaFrom (100, 3), 3);
                expectEquals (deltaFrom (100, 0), 0);
                expectEquals (deltaFrom (3, 100), 97);
                expectEquals (deltaFrom (0, 0), 0);
            }

            beginTest ("the load thresholds sit where the constants say");
            {
                expect (severityFor (0.699, 0, {}) == Severity::ok);
                expect (severityFor (0.70,  0, {}) == Severity::caution);
                expect (severityFor (0.899, 0, {}) == Severity::caution);
                expect (severityFor (0.90,  0, {}) == Severity::hot);
            }

            beginTest ("a dropout outranks a quiet CPU");
            {
                // A dropout IS the failure; load is only a predictor of one.
                expect (severityFor (0.05, 1, std::optional<juce::int64> (1000)) == Severity::hot,
                        "a recent dropout on an idle CPU is not being reported as trouble");
                expect (severityFor (0.05, 1, std::optional<juce::int64> (120000)) == Severity::caution);
                expect (severityFor (0.05, 0, {}) == Severity::ok);
            }

            beginTest ("dropouts that happened before the first poll of a session are still counted");
            {
                // FAILS IF: someone adds baseline-adopting. A device that
                // dropped audio while it was opening is exactly the case worth
                // seeing, and adopting the first reading hides the whole burst.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.1, 40, 40));

                expectEquals (monitor.dropouts().total(), 40);
            }

            beginTest ("a new device session does not inherit the previous peak or counts");
            {
                Monitor monitor;
                expect (monitor.useSession ("a"), "naming a session for the first time is a change");
                expect (! monitor.useSession ("a"), "naming the same session again is not");

                (void) monitor.observe (sampleAt (0, 0.9, 12, 12));
                expect (monitor.peakProportion() > 0.8);

                expect (monitor.useSession ("b"));
                expectEquals (monitor.dropouts().total(), 0,
                              "the new device inherited the old device's dropout count");
                expect (monitor.peakProportion() < 0.001,
                        "the new device inherited a peak measured against a different block deadline");
            }

            beginTest ("the peak is held and does not fall");
            {
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0,    0.2, 0, 0));
                (void) monitor.observe (sampleAt (500,  0.9, 0, 0));
                (void) monitor.observe (sampleAt (1000, 0.2, 0, 0));

                const auto text = monitor.readout().text;
                expect (text.contains ("20%"), "the current load is not shown");
                expect (text.contains ("90%"), "the peak fell away, so the incident nobody was "
                                               "watching leaves no trace");
            }

            beginTest ("measured-only dropouts are not escalated until there are several");
            {
                // FAILS IF: a single scheduling hiccup at a 2.7 ms buffer puts
                // "audio is dropping out" in the tray. False alarms teach
                // people to ignore the status row.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.5, 0, -1));   // settles the device

                for (int i = 1; i < kMeasuredEscalationMin; ++i)
                    expect (! monitor.observe (sampleAt (kSettleMs + i * 500, 0.5, i, -1)).report,
                            "a measured dropout below the threshold was escalated");

                expect (monitor.observe (sampleAt (kSettleMs + kMeasuredEscalationMin * 500, 0.5,
                                                   kMeasuredEscalationMin, -1)).report,
                        "the threshold was reached and nothing was reported");
            }

            beginTest ("a driver-reported dropout is escalated at once, once settled");
            {
                // The hardware noticed, so it was audible.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.1, 0, 0));

                const auto escalation = monitor.observe (sampleAt (kSettleMs + 500, 0.5, 1, 1));
                expect (escalation.report);
                expect (escalation.message.contains ("Buffer Size"),
                        "the message does not tell the user what to do about it");
            }

            beginTest ("the under-run a device reports for its own opening is not an alarm");
            {
                // MEASURED, not hypothesised: every launch on the development
                // machine produced dropouts=1 driver=1 inside the first poll,
                // and the tray said "Audio is dropping out" on a healthy start.
                //
                // FAILS IF: the settling window goes. An alarm that fires on
                // every single launch is one people learn to close without
                // reading, which costs the real one its meaning.
                Monitor monitor;
                monitor.useSession ("a");

                expect (! monitor.observe (sampleAt (0, 0.0, 1, 1)).report,
                        "the under-run a device reports for opening itself raised an alarm, "
                        "so the tray cries wolf on every launch");

                expectEquals (monitor.dropouts().total(), 1,
                              "the settling dropout was hidden rather than merely un-alarmed; "
                              "it still belongs in the log and the readout");
            }

            beginTest ("a dropout after the settling window is still an alarm");
            {
                // The other half: settling must not become a permanent excuse.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.0, 1, 1));          // settling

                expect (monitor.observe (sampleAt (kSettleMs + 1, 0.0, 2, 2)).report,
                        "a real dropout after the device settled went unreported");
            }

            beginTest ("the window runs from the chain being ready, not from the device opening");
            {
                // MEASURED. The first version of this anchored on the device
                // session alone and STILL fired on every launch: loading a real
                // chain took longer than the window, so by the time audio
                // actually flowed the device had been open for well over three
                // seconds and the under-run that came with the first block
                // counted as news.
                //
                // FAILS IF: the anchor goes back to the device alone.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.0, 0, 0));

                // Ten seconds of loading plugins, then the graph starts.
                monitor.markChainReady (10000);

                expect (! monitor.observe (sampleAt (10100, 0.0, 1, 1)).report,
                        "the under-run that arrives with a newly-built graph raised an alarm, "
                        "so a slow-loading chain cries wolf on every launch");

                expect (monitor.observe (sampleAt (10000 + kSettleMs + 1, 0.0, 2, 2)).report,
                        "a real dropout after the chain settled went unreported");
            }

            beginTest ("a machine dropping continuously cannot fill the status sink");
            {
                // The headline case. An hour of a misbehaving machine, into a
                // real Sink that already holds an unrelated problem.
                //
                // FAILS IF: the eight slots are emptied of everything else by
                // one bad afternoon, so the feature that reports the trouble
                // destroys the evidence of everything else.
                lighthost::status::Sink sink;
                sink.report ("Plugin load failed: something else entirely");

                Monitor monitor;
                monitor.useSession ("a");

                int total = 0;

                for (int tick = 0; tick < 7200; ++tick)      // one hour at 500 ms
                {
                    total += 3;

                    if (const auto escalation = monitor.observe (
                            sampleAt (tick * 500, 0.99, total, total));
                        escalation.report)
                        sink.report (escalation.message);
                }

                expect (static_cast<int> (sink.all().size()) <= 1 + kMaxReportsPerEpisode,
                        "an hour of dropouts pushed more messages than the budget allows");

                expect (sink.all().front().message.contains ("something else entirely"),
                        "the unrelated problem the user already had was evicted by the "
                        "dropout reports");
            }

            beginTest ("a repeat is not reported when the dropouts stopped");
            {
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.5, 0, 0));
                expect (monitor.observe (sampleAt (kSettleMs + 500, 0.5, 1, 1)).report);

                // Budget available and the spacing elapsed, but the count has
                // not moved. Spacing alone would repeat a stale message.
                expect (! monitor.observe (sampleAt (kRepeatMs + 10000, 0.5, 1, 1)).report,
                        "a message repeated about dropouts that had stopped");
            }

            beginTest ("a second episode after a quiet minute is reported again");
            {
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.5, 0, 0));
                expect (monitor.observe (sampleAt (kSettleMs + 500, 0.5, 1, 1)).report);

                const auto later = kSettleMs + 500 + kEpisodeQuietMs + 1000;
                const auto escalation = monitor.observe (sampleAt (later, 0.5, 2, 2));

                expect (escalation.report, "a fresh incident an hour later went unreported");
                expectEquals (monitor.reportsThisEpisode(), 1,
                              "the new episode did not get a fresh budget");
            }

            beginTest ("no device reads as no device, not as nought per cent");
            {
                Monitor monitor;
                monitor.useSession ("a");

                Sample closed;
                closed.deviceOpen = false;

                expect (! monitor.observe (closed).report);
                expect (monitor.readout().text.contains ("no audio device"));
                expect (monitor.readout().severity == Severity::ok,
                        "a closed device is being drawn as a problem");
            }

            beginTest ("a device closing for a moment does not throw away the peak");
            {
                // Devices close briefly during a change, and the peak measured
                // just before is the number that explains what happened. This
                // is half the rule; the other half is in IconMenu::pollAudioLoad,
                // which must not name a session from a setup with no device in
                // it -- doing so restarted the peak on every device change.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.95, 4, 4));

                Sample closed;
                closed.deviceOpen = false;
                (void) monitor.observe (closed);

                expect (monitor.peakProportion() > 0.9,
                        "a momentary device close discarded the peak that explains "
                        "what just went wrong");
                expectEquals (monitor.dropouts().total(), 4,
                              "a momentary device close discarded the dropout count");
            }

            beginTest ("the log line names every field");
            {
                // FAILS IF: a field is dropped and the smoke test's substring
                // marker still matches, so the gate stays green on a line that
                // no longer says anything.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.4, 3, 1));

                const auto fields = monitor.logFields();

                for (const auto* key : { "load=", "peak=", "dropouts=", "driver=",
                                         "measured=", "driverXruns=", "deadline=" })
                    expect (fields.contains (key),
                            juce::String ("the log line lost its ") + key + " field");
            }

            beginTest ("the core the audio thread ran on reaches the log line");
            {
                // FAILS IF: the core label is computed and then discarded, which
                // is what happened for one commit -- DeviceTap sampled the
                // processor every 64 blocks, ProcessFacts interpreted it, and
                // the answer reached nothing a person could read. The whole
                // point of the sampling is the one investigation it supports.
                Monitor monitor;
                monitor.useSession ("a");

                auto sample = sampleAt (0, 0.4, 0, 0);
                sample.coreLabel = "E";
                (void) monitor.observe (sample);

                expect (monitor.logFields().contains ("core=E"),
                        "the audio thread's core was sampled and never reported, so the "
                        "efficiency-core question still cannot be answered from a log");
            }

            beginTest ("no core label leaves no empty field behind");
            {
                // Empty off Windows, empty on a CPU with one core class, and
                // empty until the thread has been sampled. "core=" with nothing
                // after it reads like a field that failed.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.4, 0, 0));

                expect (! monitor.logFields().contains ("core="),
                        "an empty core label was printed as a field with no value");
            }

            beginTest ("the first report is the one that says what to do about it");
            {
                // FAILS IF: the first/last choice is keyed off "is this the
                // last" rather than "is this the first". At a
                // kMaxReportsPerEpisode of 1 -- a one-line edit to a constant
                // documented as a tuning knob -- that makes the only message a
                // user ever gets the one with no advice in it.
                Monitor monitor;
                monitor.useSession ("a");
                (void) monitor.observe (sampleAt (0, 0.5, 0, 0));

                const auto first = monitor.observe (sampleAt (kSettleMs + 500, 0.5, 1, 1));
                expect (first.report);
                expect (first.message.contains ("Raise Buffer Size"),
                        "the first dropout report does not tell the user what to try");
                expect (! first.message.contains ("last report"),
                        "the first report announced itself as the last one");
            }

            beginTest ("a monitor that has seen nothing says so rather than reporting zero");
            {
                const Monitor monitor;
                expectEquals (monitor.logFields(), juce::String ("device=none"));
            }

            beginTest ("an absent core label adds no separator");
            {
                const auto joined = composeReadout ("40%", "no dropouts", {});
                expect (! joined.endsWith ("-"), "a trailing separator is drawn for a core "
                                                 "label that does not exist");
                expect (composeReadout ("40%", "no dropouts", "P").contains ("core P"));
            }
        }
    };

    static LoadPolicyTests loadPolicyTests;
}
