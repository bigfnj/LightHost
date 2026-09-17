#include <juce_events/juce_events.h>

//==============================================================================
// Console entry point for the LightHost unit tests.
//
// Uses juce::UnitTestRunner rather than pulling in a third-party test framework:
// JUCE is already vendored, so this adds no new dependency.
//
// Runs only our own categories. JUCE's internal suite is not compiled in (see
// the JUCE_UNIT_TESTS note in CMakeLists.txt), so runAllTests() would be
// equivalent today, but naming the category keeps that true if a future module
// define drags the JUCE tests back in.
//
// Exits non-zero if any test failed, and names every failing test so a CI log is
// readable without re-running locally.
//==============================================================================
namespace
{
    /** Routes juce::Logger output to stdout.

        Without this, UnitTest::logMessage goes to Logger::outputDebugString,
        which on Windows is OutputDebugString and therefore invisible in a
        terminal or a CI log. Diagnostics that cannot be read are not
        diagnostics.
    */
    class ConsoleLogger final : public juce::Logger
    {
    public:
        void logMessage (const juce::String& message) override
        {
            std::printf ("%s\n", message.toRawUTF8());
            std::fflush (stdout);
        }
    };
}

int main (int argc, char** argv)
{
    // Two sets, because they need different environments. The default set runs
    // anywhere; --gui constructs real windows and is registered as a separate
    // CTest entry behind the xvfb check, so it never runs on a headless runner.
    const juce::String requested = argc > 1 ? juce::String (argv[1]) : juce::String();

    if (requested.isNotEmpty() && requested != "--gui")
    {
        // Refused rather than ignored. Ignoring it ran the headless set and
        // exited 0, so a typo in the --gui argument in CMakeLists.txt would have
        // turned the unit-gui entry into a silent duplicate of unit that passed
        // -- the same "looks exactly like success" failure the category check
        // below exists to prevent, arriving from the argument side.
        std::printf ("UNKNOWN ARGUMENT: %s (expected --gui, or none)\n",
                     requested.toRawUTF8());
        return 2;
    }

    const bool guiOnly = requested == "--gui";

    // Brings up the MessageManager. Nothing here posts messages, but JUCE
    // subsystems and the leak detector expect an initialised environment.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    ConsoleLogger consoleLogger;
    juce::Logger::setCurrentLogger (&consoleLogger);

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);   // let the run finish and report every failure

    int totalFailures = 0;
    int totalPasses   = 0;

    // Results are collected after every category, not once at the end.
    // UnitTestRunner::runTests() calls results.clear() on entry
    // (juce_UnitTest.cpp:176), so a single sweep afterwards would see only the
    // last category: every earlier category's failures would vanish and this
    // process would exit 0 with a broken build.
    const auto collectResults = [&runner, &totalFailures, &totalPasses]
    {
        for (int i = 0; i < runner.getNumResults(); ++i)
        {
            const auto* result = runner.getResult (i);

            if (result == nullptr)
                continue;

            totalFailures += result->failures;
            totalPasses   += result->passes;

            if (result->failures > 0)
            {
                std::printf ("FAILED: %s / %s (%d failed, %d passed)\n",
                             result->unitTestName.toRawUTF8(),
                             result->subcategoryName.toRawUTF8(),
                             result->failures,
                             result->passes);

                for (const auto& message : result->messages)
                    std::printf ("    %s\n", message.toRawUTF8());
            }
        }
    };

    // AudioChainList is headless, not needsDisplay. Its subject is a child
    // juce::Component that is never put on the desktop, so it does not hit the
    // X11 limit that forces PluginWindowGui to be Windows and macOS only: that
    // one creates a DocumentWindow, and JUCE's X11 backend cannot make a window
    // from a console app at all.
    const juce::StringArray headless { "AudioChainList",
                                       "ConfirmPolicy", "DevicePolicy", "Gain",
                                       "GraphRender", "GraphTopology", "InstanceName",
                                       "Metering", "NodeIds", "OfflineRender",
                                       "PluginChain", "PluginScan", "PluginState",
                                       "PluginStateVault", "PluginWindow",
                                       "SampleRate", "SelfTest", "SettingsKeys",
                                       "StartupFlags", "Status" };

    const juce::StringArray needsDisplay { "PluginWindowGui" };

    juce::StringArray declared (headless);
    declared.addArray (needsDisplay);

    //==========================================================================
    // Two checks on the two lists themselves, before a single test runs. Both
    // are about this file being wrong rather than about any test failing, and
    // both used to exit 0.

    // A NAME THAT IS BLANK OR ONLY WHITESPACE.
    //
    // Checked here rather than after the run, because running it is the
    // problem. UnitTest::getTestsInCategory returns getAllTests() for an empty
    // string (juce_UnitTest.cpp:57-58), so a blanked entry does not run nothing
    // -- it runs EVERYTHING, including PluginWindowGui, which needs a display
    // and takes a headless Linux runner down (CMakeLists.txt says why it is a
    // separate ctest entry). Measured: with headless = { "" } the suite ran the
    // lot and exited 0, and the empty-category guard below stayed silent,
    // because a run that executes every test is not short of results.
    //
    // Both lists are scanned, not just the one this mode uses: a blank in
    // needsDisplay is the same defect and would otherwise only be caught on the
    // platforms where unit-gui is registered at all.
    juce::StringArray blankCategories;

    for (int i = 0; i < declared.size(); ++i)
        if (declared[i].trim().isEmpty())
            blankCategories.add (juce::String (i < headless.size() ? "headless" : "needsDisplay")
                                     + "[" + juce::String (i < headless.size()
                                                               ? i : i - headless.size()) + "]");

    // A CATEGORY NOBODY LISTED.
    //
    // Add a test class with a new category, forget to add it above, and it
    // silently never runs while the suite exits 0. Nothing else notices: the
    // empty-category guard fires for a listed name with no tests, which is the
    // opposite direction.
    //
    // Run in BOTH modes, against the union of both lists. The registered tests
    // are the same static objects either way, so the answer does not depend on
    // the mode -- and unit-gui is not registered on Linux at all, so a check
    // that only ran there would never run on two of the three platforms CI
    // covers. Running it in the default mode means every platform's `unit`
    // entry catches an orphan on the first push.
    // Walked over getAllTests(), NOT getAllCategories(), which is what this used
    // to do and which left the same hole one door along.
    // UnitTest::getAllCategories skips any test whose category is empty
    // (juce_UnitTest.cpp:83-92), and the base constructor defaults the category
    // to an empty String (juce_UnitTest.h:88). So a test written as
    // `juce::UnitTest ("My test")` -- the one-argument form, which compiles --
    // appeared in no category, was named by neither list, was run by no
    // runTestsInCategory call, tripped neither guard, and the process exited 0.
    // That is the identical "registers and silently never runs" failure the
    // orphan check exists to catch, arriving through a constructor overload.
    juce::StringArray orphanCategories;
    juce::StringArray uncategorisedTests;

    for (const auto* test : juce::UnitTest::getAllTests())
    {
        if (test == nullptr)
            continue;

        const auto category = test->getCategory();

        if (category.isEmpty())
        {
            uncategorisedTests.addIfNotAlreadyThere (test->getName().isNotEmpty()
                                                         ? test->getName()
                                                         : juce::String ("<unnamed>"));
            continue;
        }

        if (! declared.contains (category))
            orphanCategories.addIfNotAlreadyThere (category);
    }

    if (! blankCategories.isEmpty()
        || ! orphanCategories.isEmpty()
        || ! uncategorisedTests.isEmpty())
    {
        if (! blankCategories.isEmpty())
            std::printf ("BLANK CATEGORY NAME: %s\n"
                         "    An empty category runs every test in the binary, not "
                         "none of them, so this cannot be run as written.\n",
                         blankCategories.joinIntoString (", ").toRawUTF8());

        if (! orphanCategories.isEmpty())
            std::printf ("CATEGORY NOT LISTED IN TestMain.cpp: %s\n"
                         "    A test registers it and neither list names it, so it "
                         "never runs and this process would still exit 0.\n",
                         orphanCategories.joinIntoString (", ").toRawUTF8());

        if (! uncategorisedTests.isEmpty())
            std::printf ("TEST REGISTERED WITH NO CATEGORY: %s\n"
                         "    juce::UnitTest's category argument defaults to empty, so "
                         "the one-argument constructor compiles and\n"
                         "    produces a test that no category names and no "
                         "runTestsInCategory call reaches. Give it a category\n"
                         "    and add that category to a list above.\n",
                         uncategorisedTests.joinIntoString (", ").toRawUTF8());

        juce::Logger::setCurrentLogger (nullptr);
        return 1;
    }

    juce::StringArray emptyCategories;

    for (const auto& category : (guiOnly ? needsDisplay : headless))
    {
        runner.runTestsInCategory (category);

        // Per category, not just overall. Categories are matched by string, so a
        // renamed or misspelled one runs nothing -- and the total-based check
        // below only notices when EVERY category is empty, which means a single
        // misspelling in the list above used to pass silently. (This sentence
        // used to say "one misspelling among fifteen"; the list has grown twice
        // since, so the count is gone rather than left wrong.)
        if (runner.getNumResults() == 0)
            emptyCategories.add (category);

        collectResults();
    }

    std::printf ("\n==== %d passed, %d failed ====\n", totalPasses, totalFailures);

    if (! emptyCategories.isEmpty())
    {
        std::printf ("NO TESTS IN CATEGORY: %s\n"
                     "    Either the name is misspelled here or the test that "
                     "registers it was removed.\n",
                     emptyCategories.joinIntoString (", ").toRawUTF8());
        juce::Logger::setCurrentLogger (nullptr);
        return 1;
    }

    if (totalPasses == 0 && totalFailures == 0)
    {
        std::printf ("NO ASSERTIONS RAN\n");
        juce::Logger::setCurrentLogger (nullptr);
        return 1;
    }

    juce::Logger::setCurrentLogger (nullptr);

    return totalFailures == 0 ? 0 : 1;
}
