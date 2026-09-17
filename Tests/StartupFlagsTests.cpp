#include "../Source/StartupFlags.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// The ordinary-run command-line flags.
//
// Small, and worth having anyway: -preferences was the one flag parsed inline
// in HostStartup rather than through a function a test could call, and the
// thing that went unnoticed as a result was not the parse but the SPELLING --
// it accepted a single dash only, while every other flag in the application
// takes either.
//==============================================================================
namespace
{

class StartupFlagsTests final : public juce::UnitTest
{
public:
    StartupFlagsTests()
        : juce::UnitTest ("Startup flags", "StartupFlags") {}

    void runTest() override
    {
        using namespace lighthost::startup;

        beginTest ("-preferences is recognised in both spellings");
        {
            // FAILS IF: the double-dash form is dropped again. --render and
            // --scan both use two dashes, so it is the natural thing to type,
            // and the inline parse this replaced silently ignored it: the app
            // started with no window and said nothing.
            expect (openPreferencesRequested ({ "-preferences" }));
            expect (openPreferencesRequested ({ "--preferences" }));
        }

        beginTest ("-preferences is found beside other flags, in any position");
        {
            expect (openPreferencesRequested ({ "-multi-instance=rig", "-preferences" }));
            expect (openPreferencesRequested ({ "-preferences", "-multi-instance=rig" }));
        }

        beginTest ("nothing else turns it on");
        {
            expect (! openPreferencesRequested ({}));
            expect (! openPreferencesRequested ({ "-self-test" }));
            expect (! openPreferencesRequested ({ "--scan" }));

            // The same two shapes the other flags' tests rule out: a bare word
            // is not a flag, and an =value form is a different token.
            expect (! openPreferencesRequested ({ "preferences" }),
                    "a bare word is not the flag");
            expect (! openPreferencesRequested ({ "-preferences=1" }),
                    "an =value form is a different token and is not this flag");
        }
    }
};

StartupFlagsTests startupFlagsTests;

} // namespace
