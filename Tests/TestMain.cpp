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
int main()
{
    // Brings up the MessageManager. Nothing here posts messages, but JUCE
    // subsystems and the leak detector expect an initialised environment.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);   // let the run finish and report every failure

    runner.runTestsInCategory ("PDC");

    int totalFailures = 0;
    int totalPasses   = 0;

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

    std::printf ("\n==== %d passed, %d failed ====\n", totalPasses, totalFailures);

    return totalFailures == 0 ? 0 : 1;
}
