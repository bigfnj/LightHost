#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Headless startup/shutdown self-test.
//
// Runs the real application: real AudioDeviceManager, real AudioProcessorGraph,
// real tray icon, real message loop, real teardown. Then it inspects its own log
// and exits non-zero if anything went wrong.
//
// This is the only automated check that exercises IconMenu's destructor ordering,
// where the audio callback must be detached before player and graph are torn
// down. See the comment in ~IconMenu.
//
// It is NOT a substitute for the manual shutdown test. On a CI runner with no
// audio device, getCurrentAudioDevice() is null and no callback thread ever
// starts, so the race that made that ordering necessary cannot occur here. This
// proves the ordering code runs without faulting; it does not reproduce the race.
//==============================================================================
namespace lighthost::selftest
{
    /** True when -self-test appears on the command line. */
    [[nodiscard]] inline bool isRequested (const juce::StringArray& params)
    {
        return params.contains ("-self-test");
    }

    /** A settings folder name unique to this run, so a self-test can never read
        or overwrite the real Light Host configuration. Returned rather than
        derived twice, so the caller can clean it up.
    */
    [[nodiscard]] inline juce::String makeRunFolderName()
    {
        return "LightHost-selftest-" + juce::Uuid().toDashedString();
    }

    /** Checks that survive being run mid-flight, before shutdown. */
    [[nodiscard]] inline juce::StringArray checkAfterStartup (const juce::File& logFile,
                                                              const juce::File& settingsFile)
    {
        juce::StringArray failures;

        if (! logFile.existsAsFile())
        {
            failures.add ("log file was never created at " + logFile.getFullPathName());
            return failures;   // nothing below can be checked
        }

        const auto log = logFile.loadFileAsString();

        if (! log.contains ("Light Host"))
            failures.add ("log is missing the startup banner");

        if (! log.contains ("IconMenu: constructing"))
            failures.add ("IconMenu never reported construction");

        if (! log.contains ("AudioConfig [startup]"))
            failures.add ("audio configuration was never logged, so device init did not complete");

        // The settings file is written lazily, so its absence here is not a
        // failure. Its parent directory existing proves the redirection worked.
        if (! settingsFile.getParentDirectory().isDirectory())
            failures.add ("settings directory was not created at "
                          + settingsFile.getParentDirectory().getFullPathName());

        return failures;
    }

    /** Checks that need the full teardown to have happened and the log closed. */
    [[nodiscard]] inline juce::StringArray checkAfterShutdown (const juce::File& logFile)
    {
        juce::StringArray failures;

        if (! logFile.existsAsFile())
        {
            failures.add ("log file disappeared before shutdown checks");
            return failures;
        }

        const auto log = logFile.loadFileAsString();

        if (! log.contains ("IconMenu: shutting down"))
            failures.add ("IconMenu did not report shutdown, so teardown did not complete");

        if (! log.contains ("PluginHostApp: shutdown"))
            failures.add ("the application did not reach its shutdown handler");

        // JUCE_LOG_ASSERTIONS=1 routes jassert failures into the log. Without it
        // they are invisible: jassertfalse only breaks when a debugger is
        // attached, so on CI an assertion would otherwise pass silently.
        if (log.contains ("JUCE Assertion failure"))
        {
            for (const auto& line : juce::StringArray::fromLines (log))
                if (line.contains ("JUCE Assertion failure"))
                    failures.add ("assertion fired: " + line.trim());
        }

        return failures;
    }

    /** Prints the verdict and returns the process exit code. */
    inline int report (const juce::StringArray& failures, const juce::File& runFolder)
    {
        if (failures.isEmpty())
        {
            std::printf ("SELF-TEST PASS\n");
            std::fflush (stdout);
            runFolder.deleteRecursively();
            return 0;
        }

        std::printf ("SELF-TEST FAIL (%d)\n", failures.size());

        for (const auto& failure : failures)
            std::printf ("  - %s\n", failure.toRawUTF8());

        // Deliberately left on disk for diagnosis.
        std::printf ("artefacts kept at: %s\n", runFolder.getFullPathName().toRawUTF8());
        std::fflush (stdout);

        return 1;
    }
}
