#include "../Source/TrayClick.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// The tray click policy: which event opens what.
//
// The behaviour under test is an ORDER. Preferences opens on the release, not
// the press, because on Windows the press arrives before the process may take
// the foreground, and a window opened then lands behind the active one. A test
// that only checked "a left click opens Preferences" would pass with the open
// on either event, and 5.6.1 was exactly that: the right window, at the wrong
// moment.
//==============================================================================
namespace
{

class TrayClickTests final : public juce::UnitTest
{
public:
    TrayClickTests() : juce::UnitTest ("Tray click policy", "TrayClick") {}

    void runTest() override
    {
        using namespace lighthost::tray;

        beginTest ("on Windows a left click opens Preferences on the release, not the press");
        {
            // FAILS IF: the open moves back to mouseDown. That is the 5.6.1
            // defect: the shell grants foreground rights around the button-up,
            // so a window shown from the press cannot be activated.
            ClickTracker tracker (true);
            expect (tracker.onPress (true) == Action::none,
                    "the press must only remember the button");
            expect (tracker.onRelease() == Action::openPreferences,
                    "the release is what opens the window");
        }

        beginTest ("the release is one-shot");
        {
            // A double-click delivers two releases for the one press JUCE
            // forwards, and the shell can deliver a release with no press.
            ClickTracker tracker (true);
            expect (tracker.onRelease() == Action::none,
                    "a release with no press does nothing");

            (void) tracker.onPress (true);
            (void) tracker.onRelease();
            expect (tracker.onRelease() == Action::none,
                    "the second release of a double-click does nothing");
        }

        beginTest ("any other button shows the menu at once, and drops a pending left");
        {
            ClickTracker tracker (true);
            expect (tracker.onPress (false) == Action::showMenu);
            expect (tracker.onRelease() == Action::none,
                    "the menu's own release opens nothing");

            (void) tracker.onPress (true);
            expect (tracker.onPress (false) == Action::showMenu,
                    "a right press during a left press still shows the menu");
            expect (tracker.onRelease() == Action::none,
                    "and the left press it interrupted is forgotten");
        }

        beginTest ("off Windows the left press opens Preferences at once, as before 5.6.2");
        {
            ClickTracker tracker (false);
            expect (tracker.onPress (true) == Action::openPreferences);
            expect (tracker.onRelease() == Action::none,
                    "nothing is pending, so the release does nothing");
            expect (tracker.onPress (false) == Action::showMenu);
        }
    }
};

TrayClickTests trayClickTests;

}
