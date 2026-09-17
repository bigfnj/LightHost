#include "../Source/SelfTest.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// The verdict the smoke tests reach, checked without running one.
//
// WHY THIS FILE EXISTS
//
// Three of the five ctest entries -- smoke-startup, smoke-repeat and
// smoke-multi-instance -- are the functions in Source/SelfTest.hpp reading a
// log file and returning a list of failures. Nothing tested those functions.
// A gate whose own verdict is untested is a gate that can start agreeing with
// everything, and it would look exactly like three passing tests.
//
// isEnvironmentalAssertion is the worst of the three, because it is the one
// with a reason to be widened. It exists so that a headless Linux runner with
// no ALSA sequencer does not go permanently red on an assertion inside JUCE's
// own MIDI client, which Light Host never asks for. It is one substring on
// purpose. Broaden it to line.contains (".cpp") -- the obvious "fix" the next
// time some other assertion is in the way -- and every JUCE assertion in the
// project's own code stops failing the smoke tests, for ever, silently. That
// single edit is what most of this file is aimed at.
//
// checkAfterStartup and checkAfterShutdown take a juce::File and read it, so
// they are testable against synthetic log content with no application, no audio
// device and no message loop. They are checked one marker at a time rather than
// against one complete log: a single "the log looks right" assertion would keep
// passing with seven of the eight checks deleted.
//==============================================================================
namespace
{

class SelfTestChecksTests final : public juce::UnitTest
{
public:
    SelfTestChecksTests()
        : juce::UnitTest ("Self-test verdict", "SelfTest") {}

    void runTest() override
    {
        using namespace lighthost::selftest;

        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("LightHostSelfTestChecks-"
                                             + juce::String (juce::Random::getSystemRandom()
                                                                 .nextInt (1 << 30)));

        const auto logFile      = root.getChildFile ("LightHost.log");
        const auto settingsFile = root.getChildFile ("settings").getChildFile ("LightHost.settings");

        root.createDirectory();
        settingsFile.getParentDirectory().createDirectory();

        // One line per check inside checkAfterStartup, in the order it makes
        // them. Written out here rather than borrowed from the production side,
        // for the same reason SettingsKeysTests pins its strings: these are what
        // the running application really logs, and a check that stopped matching
        // it would report a healthy startup as broken -- or, far worse, keep
        // matching a line nobody writes any more and report nothing at all.
        const juce::StringArray startupMarkers
        {
            "Light Host 5.2.0 starting",
            "IconMenu: constructing",
            "AudioConfig [startup] 48000 Hz, 480 samples",
            "migrated chain settings (1 plugin)",
            "Problem: Plugin load failed: SelfTest Plugin",
            "loadActivePlugins complete",
            "signal view opened; inserting probes",
            "opening Preferences window"
        };

        const juce::StringArray shutdownMarkers
        {
            "IconMenu: shutting down",
            "PluginHostApp: shutdown"
        };

        //======================================================================
        beginTest ("a complete startup log reports nothing");
        {
            logFile.replaceWithText (startupMarkers.joinIntoString ("\n"));

            const auto failures = checkAfterStartup (logFile, settingsFile);

            expectEquals (failures.size(), 0, failures.joinIntoString ("; "));
        }

        beginTest ("every startup marker is checked, one at a time");
        {
            // Drop exactly one line and exactly one failure must appear. This
            // is what stops a check being deleted, or being made to match
            // something that is always present.
            for (int missing = 0; missing < startupMarkers.size(); ++missing)
            {
                auto partial = startupMarkers;
                partial.remove (missing);
                logFile.replaceWithText (partial.joinIntoString ("\n"));

                const auto failures = checkAfterStartup (logFile, settingsFile);

                expectEquals (failures.size(), 1,
                              "dropping \"" + startupMarkers[missing]
                                  + "\" should fail exactly one check; it reported: "
                                  + (failures.isEmpty() ? juce::String ("nothing")
                                                        : failures.joinIntoString ("; ")));
            }
        }

        beginTest ("an empty startup log fails every check rather than none");
        {
            // The file exists and says nothing, which is what a run that died
            // before its first log line leaves behind. Every marker is missing,
            // so every check has to fire -- an empty log passing would be the
            // whole gate reporting success for a process that never started.
            logFile.replaceWithText ("");

            const auto failures = checkAfterStartup (logFile, settingsFile);

            expectEquals (failures.size(), startupMarkers.size(),
                          "an empty log reported " + juce::String (failures.size())
                              + " of " + juce::String (startupMarkers.size()) + " problems");
        }

        beginTest ("a log that was never created stops the startup checks at once");
        {
            // It returns early, because every check below it would otherwise
            // report a missing marker and bury the one fact that matters.
            const auto neverWritten = root.getChildFile ("never-written.log");

            const auto failures = checkAfterStartup (neverWritten, settingsFile);

            expectEquals (failures.size(), 1, failures.joinIntoString ("; "));
            expect (failures[0].contains (neverWritten.getFullPathName()),
                    "the failure has to name the path, or there is nothing to go and look at");
        }

        beginTest ("a settings directory that was never created is reported");
        {
            // The redirection into a per-run folder is what keeps a self-test
            // from reading or overwriting the real configuration. If it did not
            // happen, the run was operating on the user's live settings and the
            // log would look entirely normal.
            logFile.replaceWithText (startupMarkers.joinIntoString ("\n"));

            const auto elsewhere = root.getChildFile ("no-such-folder")
                                       .getChildFile ("LightHost.settings");

            const auto failures = checkAfterStartup (logFile, elsewhere);

            expectEquals (failures.size(), 1, failures.joinIntoString ("; "));
            expect (failures[0].contains (elsewhere.getParentDirectory().getFullPathName()));
        }

        //======================================================================
        beginTest ("a complete shutdown log reports nothing");
        {
            logFile.replaceWithText (shutdownMarkers.joinIntoString ("\n"));

            const auto failures = checkAfterShutdown (logFile);

            expectEquals (failures.size(), 0, failures.joinIntoString ("; "));
        }

        beginTest ("every shutdown marker is checked, one at a time");
        {
            // These two are the only automated exercise of IconMenu's
            // destructor ordering, so losing one of them loses the check that
            // teardown happened at all.
            for (int missing = 0; missing < shutdownMarkers.size(); ++missing)
            {
                auto partial = shutdownMarkers;
                partial.remove (missing);
                logFile.replaceWithText (partial.joinIntoString ("\n"));

                const auto failures = checkAfterShutdown (logFile);

                expectEquals (failures.size(), 1,
                              "dropping \"" + shutdownMarkers[missing]
                                  + "\" should fail exactly one check; it reported: "
                                  + (failures.isEmpty() ? juce::String ("nothing")
                                                        : failures.joinIntoString ("; ")));
            }
        }

        beginTest ("a log that disappeared before shutdown is reported, not ignored");
        {
            const auto gone = root.getChildFile ("deleted-mid-run.log");

            const auto failures = checkAfterShutdown (gone);

            expectEquals (failures.size(), 1, failures.joinIntoString ("; "));
        }

        //======================================================================
        beginTest ("an assertion in the project's own code fails the run");
        {
            auto lines = shutdownMarkers;
            lines.add (assertionIn ("IconMenu.cpp", 412));
            logFile.replaceWithText (lines.joinIntoString ("\n"));

            const auto failures = checkAfterShutdown (logFile);

            expectEquals (failures.size(), 1, failures.joinIntoString ("; "));
            expect (failures[0].contains ("IconMenu.cpp:412"),
                    "the failure must carry the assertion site, or the log has to be "
                    "opened by hand to find out what fired");
        }

        beginTest ("the one environmental assertion does not fail the run");
        {
            // A headless runner has no ALSA sequencer, snd_seq_open fails, and
            // JUCE asserts inside its own Linux MIDI client. Light Host does no
            // MIDI routing and never asks for a MIDI device, so failing on it
            // would leave Linux permanently red -- which is the fastest way to
            // teach everyone to ignore a red build.
            auto lines = shutdownMarkers;
            lines.add (assertionIn ("juce_Midi_linux.cpp", 539));
            logFile.replaceWithText (lines.joinIntoString ("\n"));

            const auto failures = checkAfterShutdown (logFile);

            expectEquals (failures.size(), 0, failures.joinIntoString ("; "));
        }

        beginTest ("an environmental assertion does not excuse a real one beside it");
        {
            // The dangerous shape: both fire in one run, and the exemption is
            // applied per line rather than to the whole log.
            auto lines = shutdownMarkers;
            lines.add (assertionIn ("juce_Midi_linux.cpp", 539));
            lines.add (assertionIn ("PluginWindow.cpp", 88));
            lines.add (assertionIn ("juce_Midi_linux.cpp", 539));
            logFile.replaceWithText (lines.joinIntoString ("\n"));

            const auto failures = checkAfterShutdown (logFile);

            expectEquals (failures.size(), 1, failures.joinIntoString ("; "));
            expect (failures[0].contains ("PluginWindow.cpp:88"));
        }

        beginTest ("several real assertions are reported separately");
        {
            // One failure per line, not one saying "assertions fired". Two
            // different sites is two different bugs.
            auto lines = shutdownMarkers;
            lines.add (assertionIn ("IconMenu.cpp", 412));
            lines.add (assertionIn ("PreferencesWindow.cpp", 77));
            logFile.replaceWithText (lines.joinIntoString ("\n"));

            const auto failures = checkAfterShutdown (logFile);

            expectEquals (failures.size(), 2, failures.joinIntoString ("; "));
        }

        //======================================================================
        beginTest ("the environmental exemption is exactly one file");
        {
            expect (isEnvironmentalAssertion (assertionIn ("juce_Midi_linux.cpp", 539)),
                    "the ALSA sequencer assertion is the one case this exists for");

            // THE MUTATION THIS TEST IS FOR. Widen the predicate to
            // line.contains (".cpp") and three ctest entries pass for ever.
            // Every file below is one this project writes, and an assertion in
            // any of them has to stay a failure.
            for (const auto* ours : { "IconMenu.cpp", "PluginWindow.cpp",
                                      "PreferencesWindow.cpp", "HostStartup.cpp" })
                expect (! isEnvironmentalAssertion (assertionIn (ours, 120)),
                        juce::String (ours) + " is this project's own code, and an "
                                              "assertion in it must fail the run");

            // Nor is the exemption "anything from the vendored JUCE tree". The
            // graph and the component hierarchy are where a real bug in this
            // host shows up as somebody else's assertion.
            for (const auto* theirs : { "juce_AudioProcessorGraph.cpp",
                                        "juce_Component.cpp",
                                        "juce_AudioDeviceManager.cpp",
                                        "juce_Midi_windows.cpp" })
                expect (! isEnvironmentalAssertion (assertionIn (theirs, 900)),
                        juce::String (theirs) + " is not the exempted file");
        }

        beginTest ("the exemption is a filename, not a topic");
        {
            // A predicate matching "midi" or "linux" would excuse assertions
            // that have nothing to do with the ALSA sequencer.
            expect (! isEnvironmentalAssertion (""));
            expect (! isEnvironmentalAssertion ("JUCE Assertion failure in midi_linux:539"));
            expect (! isEnvironmentalAssertion ("a log line that mentions midi on linux"));
            expect (! isEnvironmentalAssertion (assertionIn ("juce_Midi_linux.h", 539)),
                    "the header is not the file the assertion comes from");
        }

        root.deleteRecursively();
    }

private:
    /** One line exactly as JUCE_LOG_ASSERTIONS writes it.

        juce::logAssertion (juce_Logger.cpp:61-66) logs the FILENAME only, not
        the path, so a synthetic log built from full paths would be testing a
        shape that never reaches the file.
    */
    static juce::String assertionIn (const juce::String& file, int line)
    {
        return "JUCE Assertion failure in " + file + ":" + juce::String (line);
    }
};

SelfTestChecksTests selfTestChecksTests;

} // namespace
