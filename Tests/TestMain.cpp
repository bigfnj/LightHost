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

int main()
{
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

    for (const auto* category : { "GraphRender", "GraphTopology", "PluginChain",
                                  "PluginState", "SampleRate", "Status" })
    {
        runner.runTestsInCategory (category);
        collectResults();
    }

    std::printf ("\n==== %d passed, %d failed ====\n", totalPasses, totalFailures);

    // A category is matched by string. A renamed or misspelled one silently runs
    // nothing, which would otherwise look exactly like success.
    if (totalPasses == 0 && totalFailures == 0)
    {
        std::printf ("NO ASSERTIONS RAN: a category above matches no registered test\n");
        juce::Logger::setCurrentLogger (nullptr);
        return 1;
    }

    juce::Logger::setCurrentLogger (nullptr);

    return totalFailures == 0 ? 0 : 1;
}
