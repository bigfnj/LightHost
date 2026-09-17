#include "../Source/SettingsKeys.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// The settings keys are an ON-DISK CONTRACT, so this file pins the literal
// strings rather than checking that the constants equal themselves.
//
// That looks like a tautology and is not. Every released version of Light Host
// has written a settings file containing these exact names, and a user
// upgrading reads their old file with the new binary. Renaming one for
// tidiness would not fail to compile, would not fail any other test, and would
// silently present the user with an empty plugin list, an empty chain and a
// default audio device -- looking, from the outside, exactly like a first run.
//
// So the test asserts what is on disk. If a rename is genuinely wanted, it has
// to come with a migration, and changing this file is the moment that has to be
// noticed.
//==============================================================================
class SettingsKeysTests final : public juce::UnitTest
{
public:
    SettingsKeysTests() : juce::UnitTest ("Settings keys", "SettingsKeys") {}

    void runTest() override
    {
        using namespace lighthost;

        beginTest ("the published key names are exactly these strings");
        {
            expectEquals (juce::String (keys::pluginList),       juce::String ("pluginList"));
            expectEquals (juce::String (keys::pluginListActive), juce::String ("pluginListActive"));
            expectEquals (juce::String (keys::audioDeviceState), juce::String ("audioDeviceState"));
            expectEquals (juce::String (keys::requestedDevices), juce::String ("requestedAudioDevices"));
        }

        beginTest ("no key collides with another");
        {
            const juce::StringArray all { keys::pluginList,
                                          keys::pluginListActive,
                                          keys::audioDeviceState,
                                          keys::requestedDevices };

            juce::StringArray unique = all;
            unique.removeDuplicates (false);

            expectEquals (unique.size(), all.size(),
                          "two settings keys share a name, so one overwrites the other");
        }

        beginTest ("no key starts with the legacy chain prefix");
        {
            // chain::Store::purgeLegacyKeys deletes everything beginning
            // "plugin-" after a migration. A key here that matched would be
            // swept away on the first launch of a new version, taking the
            // user's plugin list or device choice with it. The dash is what
            // keeps "pluginList" safe, and it is easy to lose.
            for (const auto* key : { keys::pluginList,
                                     keys::pluginListActive,
                                     keys::audioDeviceState,
                                     keys::requestedDevices })
                expect (! juce::String (key).startsWith ("plugin-"),
                        juce::String (key) + " would be deleted by the legacy-key purge");
        }

        beginTest ("no key is empty or carries whitespace");
        {
            // A PropertySet will happily store "" or " pluginList", and the
            // read side would then never find it.
            for (const auto* key : { keys::pluginList,
                                     keys::pluginListActive,
                                     keys::audioDeviceState,
                                     keys::requestedDevices })
            {
                const juce::String value (key);
                expect (value.isNotEmpty(), "a settings key is empty");
                expectEquals (value, value.trim(), "a settings key has surrounding whitespace");
                expect (! value.containsChar (' '), "a settings key contains a space");
            }
        }
    }
};

static SettingsKeysTests settingsKeysTests;
