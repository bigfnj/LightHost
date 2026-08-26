#pragma once

#include "PluginChainStore.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
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

    //==========================================================================
    // A seeded chain, so a self-test run exercises settings written by an earlier
    // version rather than only first-launch behaviour.
    //
    // The seeded plugin's format matches no registered format, so the host
    // reports a load failure and carries on. That needs no plugin on disk, and it
    // covers three things worth covering: the one-shot migration of 4.0.3's
    // per-plugin keys, the load-failure path, and the rule that a plugin which
    // never loaded keeps its saved state instead of having it overwritten.
    //==========================================================================
    [[nodiscard]] inline juce::PluginDescription seedDescription()
    {
        juce::PluginDescription description;
        description.name             = "SelfTest Plugin";
        description.version          = "1.0.0";
        description.pluginFormatName = "SelfTestFormat";
        description.fileOrIdentifier = "selftest://not-a-real-plugin";
        description.uniqueId         = 0x5e1f7e57;
        return description;
    }

    [[nodiscard]] inline juce::String seedState() { return "c2VsZi10ZXN0LXN0YXRl"; }

    /** The 4.0.3 key format, written out here rather than borrowed from the store,
        so this seeds what that version really wrote.
    */
    [[nodiscard]] inline juce::String seedLegacyKey (const juce::String& field)
    {
        const auto description = seedDescription();
        return "plugin-" + field + "-"
             + description.name + description.version + description.pluginFormatName;
    }

    /** Writes a one-plugin chain in the 4.0.3 format. Called before the app's own
        settings are read, so the migration sees it on startup.
    */
    inline void seedLegacyChain (juce::PropertySet& settings)
    {
        juce::KnownPluginList list;
        list.addType (seedDescription());

        if (auto xml = list.createXml())
            settings.setValue ("pluginListActive", xml.get());

        settings.setValue (seedLegacyKey ("order"),  1234);
        settings.setValue (seedLegacyKey ("lane"),   2);
        settings.setValue (seedLegacyKey ("bypass"), true);
        settings.setValue (seedLegacyKey ("nodeid"), 77);
        settings.setValue (seedLegacyKey ("state"),  seedState());
    }

    /** Asserts the seeded chain was migrated onto the stable keys with every
        value intact.
    */
    [[nodiscard]] inline juce::StringArray checkSeededChainMigrated (juce::PropertySet& settings)
    {
        using Store = lighthost::chain::Store;
        namespace fields = lighthost::chain::fields;

        juce::StringArray failures;
        const auto plugin = seedDescription();

        for (const auto* field : fields::all)
            if (settings.containsKey (seedLegacyKey (field)))
                failures.add (juce::String ("legacy key '") + field
                              + "' survived, so the chain settings were not migrated");

        const auto expectInt = [&] (const char* field, int expected, const juce::String& what)
        {
            const auto actual = settings.getIntValue (Store::keyFor (plugin, field), -1);

            if (actual != expected)
                failures.add (what + " did not survive migration: expected "
                              + juce::String (expected) + ", got " + juce::String (actual));
        };

        expectInt (fields::order,  1234, "order");
        expectInt (fields::lane,   2,    "lane");
        expectInt (fields::bypass, 1,    "bypass");
        expectInt (fields::nodeId, 77,   "node id");

        if (settings.getValue (Store::keyFor (plugin, fields::state)) != seedState())
            failures.add ("the saved plugin state did not survive migration");

        if (settings.getIntValue ("chainSettingsVersion", 0) != Store::kFormatVersion)
            failures.add ("the settings format version was not recorded, so migration will run again");

        return failures;
    }

    /** After shutdown, with the settings file closed: the seeded plugin never
        loaded, so its saved state must still be on disk. Overwriting it with a
        plugin's factory defaults is the data loss guarded against in
        PluginState.hpp, and this is the end-to-end check of it.
    */
    [[nodiscard]] inline juce::StringArray checkSeededStateSurvived (const juce::File& settingsFile)
    {
        juce::StringArray failures;

        if (! settingsFile.existsAsFile())
        {
            failures.add ("settings file was never written to " + settingsFile.getFullPathName());
            return failures;
        }

        if (! settingsFile.loadFileAsString().contains (seedState()))
            failures.add ("the saved state of a plugin that never loaded was overwritten");

        return failures;
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

        if (! log.contains ("migrated chain settings"))
            failures.add ("the seeded 4.0.3 chain was not reported as migrated");

        // The seeded plugin cannot be instantiated, and the host must say so and
        // keep going rather than stalling the load chain.
        if (! log.contains ("Plugin load failed"))
            failures.add ("a plugin that cannot be instantiated was not reported as failed");

        if (! log.contains ("loadActivePlugins complete"))
            failures.add ("the load chain did not finish after a failed plugin load");

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
