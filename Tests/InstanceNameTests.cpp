#include "../Source/InstanceName.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// The -multi-instance=NAME argument, on its way into a filename.
//==============================================================================
class InstanceNameTests final : public juce::UnitTest
{
public:
    InstanceNameTests()
        : juce::UnitTest ("Instance name", "InstanceName") {}

    void runTest() override
    {
        using namespace lighthost::instance;

        beginTest ("an ordinary name is left alone");
        {
            expectEquals (sanitise ("studio"), juce::String ("studio"));
            expectEquals (sanitise ("Studio-2"), juce::String ("Studio-2"));
            expectEquals (sanitise ("live_rig_3"), juce::String ("live_rig_3"));
        }

        beginTest ("no name means no second instance");
        {
            expect (sanitise ({}).isEmpty());
        }

        beginTest ("path separators cannot escape the settings folder");
        {
            // The name is concatenated into the settings filename. Unsanitised,
            // this wrote wherever the path resolved to.
            for (const auto* attempt : { "../../evil", "..\\..\\evil", "/etc/passwd",
                                         "C:\\Windows\\System32\\x" })
            {
                const auto safe = sanitise (attempt);

                expect (! safe.containsAnyOf ("/\\:."),
                        juce::String ("separators survived in '") + attempt + "' -> " + safe);
            }
        }

        beginTest ("dots alone cannot form a relative path");
        {
            expect (! sanitise ("..").containsChar ('.'));
            expectEquals (sanitise (".."), kFallbackName);
        }

        beginTest ("characters a filesystem rejects are dropped");
        {
            // A rejected character produced a file that could not be opened, and
            // since every settings write ignored its result, the configuration
            // then silently never saved.
            expectEquals (sanitise ("my*rig?"), juce::String ("myrig"));
            expectEquals (sanitise ("a<b>c|d"), juce::String ("abcd"));
            expectEquals (sanitise ("with space"), juce::String ("withspace"));
        }

        beginTest ("a name that sanitises to nothing does not land on the main settings");
        {
            // Falling back to an empty suffix would put a deliberately separate
            // instance on top of the primary configuration.
            expectEquals (sanitise ("???"), kFallbackName);
            expectEquals (sanitise ("///"), kFallbackName);
            expect (sanitise ("???").isNotEmpty());
        }

        beginTest ("length is bounded");
        {
            const juce::String tooLong = juce::String::repeatedString ("a", kMaxNameLength * 3);

            expectEquals (sanitise (tooLong).length(), kMaxNameLength);
        }

        beginTest ("sanitising is idempotent");
        {
            for (const auto* attempt : { "studio", "../../evil", "???", "my*rig?" })
            {
                const auto once  = sanitise (attempt);
                const auto twice = sanitise (once);

                expectEquals (twice, once, juce::String ("not idempotent for ") + attempt);
            }
        }
    }
};

static InstanceNameTests instanceNameTests;
