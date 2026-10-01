#pragma once

//==============================================================================
// What a tray-icon click does, and WHEN: on the press or on the release.
//
// On Windows the window a left-click opens has to wait for the release. The
// shell hands a notification icon's process the right to take the foreground
// (AllowSetForegroundWindow) around the button-up it forwards, not the
// button-down, and JUCE forwards the press first. A window opened from
// mouseDown is therefore shown by a process Windows will not yet let activate
// anything: JUCE's BringWindowToTop leaves it behind the active window and the
// taskbar button flashes instead. That was 5.6.1: every open from the tray
// landed under whatever the user was working in. Measured on the development
// machine on 2026-09-30, and recorded in the CHANGELOG for 5.6.2.
//
// This is the pure half. It remembers which button went down and says what
// the release means, because JUCE's Windows tray component delivers mouseUp
// with the button modifiers already cleared (juce_SystemTrayIcon_windows.cpp,
// eventMods.withoutMouseButtons()), so the release cannot tell left from
// right on its own. The Win32 half, taking the foreground once the window
// exists, is WindowForeground.hpp.
//
// Other platforms keep the press. There IconMenu::mouseDown activates the
// application itself through Process::makeForegroundProcess, which is a
// no-op on Windows (juce_Windowing_windows.cpp) and the reason the press was
// ever thought sufficient here.
//==============================================================================
namespace lighthost::tray
{
    enum class Action
    {
        none,
        openPreferences,
        showMenu
    };

    /** Turns a press/release pair on the tray icon into one Action.

        opensOnRelease is the Windows setting. With it false, the left press
        opens Preferences at once, which is what every version before 5.6.2
        did on every platform.
    */
    class ClickTracker
    {
    public:
        explicit ClickTracker (bool opensOnRelease) noexcept
            : waitForRelease (opensOnRelease) {}

        /** A mouse button went down on the icon.

            Any button other than the left shows the menu immediately: a popup
            is a temporary window that needs no activation, so it has nothing
            to wait for. The left either opens Preferences now or, when the
            platform makes us wait, is only remembered.
        */
        [[nodiscard]] Action onPress (bool leftButton) noexcept
        {
            if (! leftButton)
            {
                leftPending = false;
                return Action::showMenu;
            }

            if (! waitForRelease)
                return Action::openPreferences;

            leftPending = true;
            return Action::none;
        }

        /** A mouse button came up on the icon.

            Opens Preferences only for a left press this tracker saw, and only
            once. The shell can deliver a release with no press (a neighbouring
            icon removing itself mid-click does it), and a double-click delivers
            two releases for the one press JUCE forwards.
        */
        [[nodiscard]] Action onRelease() noexcept
        {
            const bool wasLeft = leftPending;
            leftPending = false;
            return wasLeft ? Action::openPreferences : Action::none;
        }

    private:
        bool waitForRelease;
        bool leftPending = false;
    };
}
