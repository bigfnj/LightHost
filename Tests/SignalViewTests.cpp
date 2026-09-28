#include "../Source/SignalView.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <vector>

//==============================================================================
// The signal view: which names label its rows, what the rows carry, and when
// the column is actually watching.
//
// WHY THESE CAN BE TESTED AT ALL
//
// The same reason Tests/AudioChainListTests.cpp can exist. Both rules asserted
// below were written out inside PreferencesContentComponent::refreshSignalView,
// and that class reaches JUCEApplication::getInstance(), getAppProperties() and
// a live AudioDeviceManager in its constructor -- so a unit test cannot build
// one, and nothing here had ever been checked. Source/SignalView.hpp exists so
// that it can be.
//
// HEADLESS, NOT needsDisplay. Nothing here is put on the desktop. That is the
// point of the last group: a SignalViewRows that was never added to a parent
// has isShowing() == false, which is exactly the state in which a timer must
// not be running -- and it is reachable from a console app precisely because
// no window is involved.
//
// THE RULE THESE EXIST FOR
//
// A null committed-names callback is the only fallback to the staged rows. An
// empty RESULT is not one. Both directions are asserted, because the plausible
// mis-fix -- guarding on the result being empty rather than on the callback
// being absent -- passes every other test in this file.
//==============================================================================
namespace
{
    using namespace lighthost::ui;

    constexpr int kMaxProbes = lighthost::nodeids::maxProbes;

    juce::PluginDescription describe (const juce::String& name)
    {
        juce::PluginDescription description;
        description.name             = name;
        description.version          = "1.0.0";
        description.fileOrIdentifier = "C:/VST3/" + name + ".vst3";
        description.pluginFormatName = "VST3";
        return description;
    }

    ChainRows stagedRowsFor (const juce::StringArray& names)
    {
        ChainRows rows;

        for (const auto& name : names)
            rows.push_back ({ describe (name), false, 0 });

        return rows;
    }

    std::vector<juce::String> namesFor (const juce::StringArray& names)
    {
        std::vector<juce::String> result;

        for (const auto& name : names)
            result.push_back (name);

        return result;
    }

    /** `count` distinct plugin names, for the cap tests.

        Built from kMaxProbes rather than from the literal 32, so raising the
        cap in NodeIds.hpp moves these tests with it instead of silently
        turning them into assertions about a number that no longer applies.
    */
    std::vector<juce::String> generatedNames (int count)
    {
        std::vector<juce::String> names;

        for (int i = 0; i < count; ++i)
            names.push_back ("plugin " + juce::String (i));

        return names;
    }

    juce::String joined (const std::vector<juce::String>& names)
    {
        juce::StringArray parts;

        for (const auto& name : names)
            parts.add (name);

        return parts.joinIntoString (",");
    }

    juce::String tapNames (const std::vector<SignalTap>& taps)
    {
        juce::StringArray parts;

        for (const auto& tap : taps)
            parts.add (tap.name);

        return parts.joinIntoString (",");
    }

    /** A pool of real meters to hand out as probe meters.

        Real rather than reinterpret_cast pointers, because signalViewContents
        only ever compares them against nullptr today and a test that leans on
        that would stop meaning anything the moment it reads one.
    */
    struct MeterPool
    {
        std::array<lighthost::metering::Meter, lighthost::nodeids::maxProbes> meters;

        [[nodiscard]] lighthost::metering::Meter* at (int index) noexcept
        {
            return juce::isPositiveAndBelow (index, kMaxProbes)
                       ? &meters[static_cast<size_t> (index)]
                       : nullptr;
        }
    };
}

//==============================================================================
class SignalViewRowNameTests final : public juce::UnitTest
{
public:
    SignalViewRowNameTests()
        : juce::UnitTest ("Signal view row names", "SignalView") {}

    void runTest() override
    {
        beginTest ("no callback falls back to the staged rows, in order");
        {
            // FAILS IF: the fallback goes. With no committed-chain callback
            // there is nothing else to label the rows with, and a column of
            // unnamed meters is not a diagnostic.
            const auto staged = stagedRowsFor ({ "EQ", "Comp", "Limiter" });

            const auto names = signalViewRowNames (nullptr, staged);

            expectEquals (joined (names), juce::String ("EQ,Comp,Limiter"),
                          "the staged rows did not reach the signal view, or "
                          "reached it out of order");
        }

        beginTest ("an empty committed result is not a fallback");
        {
            // FAILS IF: the guard is written against the RESULT being empty
            // rather than against the CALLBACK being absent. This is the whole
            // reason signalViewRowNames exists as a named function, and it is
            // the one case that had never been asserted.
            const auto staged = stagedRowsFor ({ "EQ", "Comp", "Limiter" });

            const std::function<std::vector<juce::String>()> emptyCommitted =
                [] { return std::vector<juce::String>{}; };

            const auto names = signalViewRowNames (emptyCommitted, staged);

            expect (names.empty(),
                    "an empty committed chain fell back to the staged rows, so "
                    "the signal view would label rows from a chain the graph is "
                    "not running");
        }

        beginTest ("a non-empty committed result is used verbatim");
        {
            // FAILS IF: the two lists are ever merged, or the staged names are
            // appended when the committed list is shorter. Either would pair a
            // name with a probe measuring a different plugin.
            const auto staged = stagedRowsFor ({ "EQ", "Comp", "Limiter" });

            const std::function<std::vector<juce::String>()> committed =
                [] { return namesFor ({ "Reverb", "Delay" }); };

            const auto names = signalViewRowNames (committed, staged);

            expectEquals (joined (names), juce::String ("Reverb,Delay"),
                          "the committed chain's names were not used as given");

            for (const auto* stagedName : { "EQ", "Comp", "Limiter" })
                expect (joined (names).contains (stagedName) == false,
                        juce::String ("the staged name \"") + stagedName
                            + "\" reached the signal view, so a row is labelled "
                              "from a chain the graph is not running");
        }

        beginTest ("a callback returning nothing is still not a fallback with no staged rows");
        {
            // Both sides empty is the degenerate case, and it must come back
            // empty rather than reaching for anything.
            const std::function<std::vector<juce::String>()> emptyCommitted =
                [] { return std::vector<juce::String>{}; };

            expect (signalViewRowNames (emptyCommitted, {}).empty());
            expect (signalViewRowNames (nullptr, {}).empty());
        }
    }
};

static SignalViewRowNameTests signalViewRowNameTests;

//==============================================================================
class SignalViewContentsTests final : public juce::UnitTest
{
public:
    SignalViewContentsTests()
        : juce::UnitTest ("Signal view contents", "SignalView") {}

    void runTest() override
    {
        beginTest ("the device rows bracket the chain, whatever the chain does");
        {
            // FAILS IF: Input or Output stops being pushed, or the chain rows
            // are put outside them. The delta column reads the row above, so
            // the first plugin's gain change is measured against Input and
            // nothing else.
            MeterPool pool;
            lighthost::metering::Meter inputMeter, outputMeter;

            for (const auto& names : { namesFor ({}),
                                       namesFor ({ "EQ" }),
                                       namesFor ({ "EQ", "Comp", "Limiter" }) })
            {
                const auto contents = signalViewContents (
                    names, "Focusrite", &inputMeter, "3.5 ms", &outputMeter,
                    [&pool] (int index) { return pool.at (index); });

                expectEquals (static_cast<int> (contents.taps.size()),
                              static_cast<int> (names.size()) + 2,
                              "the signal view is not showing one row per plugin "
                              "between the two device rows");

                expectEquals (contents.taps.front().name, juce::String ("Input"));
                expectEquals (contents.taps.back().name, juce::String ("Output"));

                expect (contents.taps.front().meter == &inputMeter,
                        "the Input row is not reading the input device's meter");
                expect (contents.taps.back().meter == &outputMeter,
                        "the Output row is not reading the output device's meter");

                expectEquals (contents.taps.front().detail, juce::String ("Focusrite"));
                expectEquals (contents.taps.back().detail, juce::String ("3.5 ms"));
            }
        }

        beginTest ("no device selected says so rather than naming nothing");
        {
            const auto contents = signalViewContents ({}, {}, nullptr, {}, nullptr,
                                                      nullptr);

            expectEquals (contents.taps.front().detail, juce::String ("no device"));
        }

        beginTest ("no chain row is ever given a name and a null meter");
        {
            // FAILS IF: the null-meter row is labelled instead of dropped. The
            // painter draws a row with no meter at the floor for ever, and the
            // reading is indistinguishable from a plugin that has stopped
            // passing audio -- which is precisely the diagnosis this column
            // exists to make.
            MeterPool pool;

            const auto names = generatedNames (kMaxProbes + 3);

            const auto contents = signalViewContents (
                names, "Focusrite", nullptr, {}, nullptr,
                [&pool] (int index) { return pool.at (index); });

            for (const auto& tap : contents.taps)
                if (tap.name != "Input" && tap.name != "Output")
                    expect (tap.meter != nullptr,
                            "a chain row was given a name and no meter, so it "
                            "will draw permanent silence and read as a plugin "
                            "passing nothing");

            expectEquals (static_cast<int> (contents.taps.size()), kMaxProbes + 2,
                          "the rows past the probe cap were kept even though "
                          "nothing measures them");
        }

        beginTest ("a null probe callback leaves the two device rows alone");
        {
            // The whole chain loses its meters, so the whole chain loses its
            // rows -- but Input and Output do not depend on the probes and
            // must survive.
            lighthost::metering::Meter inputMeter, outputMeter;

            const auto contents = signalViewContents (
                namesFor ({ "EQ", "Comp" }), "Focusrite", &inputMeter, "1.0 ms",
                &outputMeter, nullptr);

            expectEquals (tapNames (contents.taps), juce::String ("Input,Output"));
        }

        beginTest ("the probe limit is reported at the cap and not before it");
        {
            // FAILS IF: the `>=` is written in unsigned arithmetic anywhere
            // near a subtraction. Zero names is the case that catches a wrap:
            // an empty chain reported as "at the 32-probe limit" tells the user
            // rows are missing when there are none.
            MeterPool pool;
            const auto probes = [&pool] (int index) { return pool.at (index); };

            const auto none = signalViewContents (generatedNames (0), {}, nullptr,
                                                  {}, nullptr, probes);
            expect (! none.atProbeLimit,
                    "a chain with no plugins in it is being reported as sitting "
                    "on the probe limit");

            const auto justUnder = signalViewContents (generatedNames (kMaxProbes - 1),
                                                       {}, nullptr, {}, nullptr, probes);
            expect (! justUnder.atProbeLimit,
                    "a chain one short of the cap is being reported as sitting "
                    "on it, so the header warns about rows that are all present");

            const auto atCap = signalViewContents (generatedNames (kMaxProbes),
                                                   {}, nullptr, {}, nullptr, probes);
            expect (atCap.atProbeLimit,
                    "a chain sitting on the probe cap is not saying so, so a "
                    "longer chain silently loses its tail with no warning");
        }

        beginTest ("the panel reports the flag it was handed");
        {
            SignalViewPanel panel;

            expect (! panel.isAtProbeLimit());

            panel.setTaps ({}, true);
            expect (panel.isAtProbeLimit(),
                    "the column was told it is at the probe limit and is not "
                    "showing the note");

            panel.setTaps ({}, false);
            expect (! panel.isAtProbeLimit(),
                    "the probe-limit note latched, so it stays on a chain that "
                    "has since been shortened");
        }
    }
};

static SignalViewContentsTests signalViewContentsTests;

//==============================================================================
class SignalViewWatchTests final : public juce::UnitTest
{
public:
    SignalViewWatchTests()
        : juce::UnitTest ("Signal view watching", "SignalView") {}

    void runTest() override
    {
        beginTest ("replacing the taps on a hidden column stops watching entirely");
        {
            // FAILS IF: setTaps clears the watches without stopping the timer.
            // The rows here are never added to a parent, so isShowing() is
            // false and beginWatching() is not called back -- which used to
            // leave the 25 Hz callback running over an empty watch list until
            // some later tick noticed. It did recover on the next tick; that is
            // self-healing, not correct, and it depended entirely on a guard
            // inside timerCallback that nothing else asserts.
            lighthost::metering::Meter first, second;

            SignalViewRows rows;
            rows.setTaps ({ { "Input", "Focusrite", &first },
                            { "Output", "1.0 ms", &second } });

            rows.setWatching (true);
            expect (rows.isTimerActive(),
                    "setWatching(true) did not start the frame timer, so this "
                    "test cannot show that setTaps stops it");
            expectEquals (rows.numWatches(), 2);

            rows.setTaps ({ { "Input", "Focusrite", &first } });

            expect (! rows.isTimerActive(),
                    "the column is not showing and a 25 Hz callback is still "
                    "running over an empty watch list");
            expectEquals (rows.numWatches(), 0,
                          "the column is not showing and the audio thread is "
                          "still accumulating a per-sample sum of squares for it");
        }

        beginTest ("an empty tap list starts nothing");
        {
            // FAILS IF: beginWatching starts the timer regardless of whether
            // there is anything to read. A timer with no taps is pure idle cost
            // in a tray-resident application.
            SignalViewRows rows;

            rows.setTaps ({});
            rows.setWatching (true);

            expect (! rows.isTimerActive(),
                    "a column with no rows is running a 25 Hz timer with "
                    "nothing to read");
            expectEquals (rows.numWatches(), 0);
        }

        beginTest ("the taps that were set are the taps that are held");
        {
            lighthost::metering::Meter meter;

            SignalViewRows rows;
            rows.setTaps ({ { "Input", "Focusrite", &meter },
                            { "after EQ", {}, &meter },
                            { "Output", "1.0 ms", nullptr } });

            expectEquals (tapNames (rows.getTaps()),
                          juce::String ("Input,after EQ,Output"));
            expectEquals (static_cast<int> (rows.getTaps().size()), 3);
        }

        beginTest ("a tap with no meter takes no watch");
        {
            // The watch is what turns on the audio thread's per-sample RMS
            // work, so a row with nothing to read must not ask for one.
            lighthost::metering::Meter meter;

            SignalViewRows rows;
            rows.setTaps ({ { "Input", {}, &meter },
                            { "Output", {}, nullptr } });

            rows.setWatching (true);

            expectEquals (rows.numWatches(), 1,
                          "a row with no meter took a watch, so the audio "
                          "thread is measuring RMS for a row that shows none");
        }

        beginTest ("setWatching(false) stops both halves");
        {
            lighthost::metering::Meter meter;

            SignalViewRows rows;
            rows.setTaps ({ { "Input", {}, &meter } });
            rows.setWatching (true);

            expect (rows.isTimerActive());

            rows.setWatching (false);

            expect (! rows.isTimerActive());
            expectEquals (rows.numWatches(), 0);
        }
    }
};

static SignalViewWatchTests signalViewWatchTests;
