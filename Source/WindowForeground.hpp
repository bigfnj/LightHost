#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// Bringing a window opened from the tray in front of the user.
//
// JUCE's toFront (true) ends in BringWindowToTop on Windows, which reorders
// the Z-order within what the calling process is allowed to touch. The call
// that moves a process to the foreground is SetForegroundWindow, and nothing
// on JUCE's window path makes it; its one use is on the hidden tray window,
// at the press, before the shell has granted anything. So a window shown by
// a process that is not in the foreground lands behind the active window and
// Windows flashes its taskbar button, which is what 5.6.1 did on every open
// from the tray.
//
// Windows grants SetForegroundWindow only to a process that already holds the
// foreground, just received input, or was handed the right by the process
// that has it. Each caller here qualifies at the moment it calls: the tray
// release (the shell hands over the right around the button-up), a menu item
// (the click landed in our own popup), and -preferences at startup (we were
// started by the foreground process). When Windows refuses anyway, under a
// foreground lock or a policy, the window is lifted to the top of the Z-order
// without taking the focus, which any process may do to its own window, so
// the user at least sees what they asked for.
//
// <windows.h> stays inside the .cpp: IconMenu.hpp reaches most of the GUI.
// The .cpp is compiled into the application only, like ProcessQoS.cpp.
//==============================================================================
namespace lighthost::window
{
    struct RaiseResult
    {
        bool attempted  = false;   // false with no native window, and off Windows
        bool foreground = false;   // SetForegroundWindow accepted
        bool liftedOnly = false;   // refused; put on top of the Z-order without focus
        juce::uint32 errorCode = 0;

        /** One fragment for the log: "foreground", or what happened instead. */
        [[nodiscard]] juce::String describe() const;
    };

    /** Brings a desktop window in front of other applications' windows and,
        where Windows permits, makes it the active window.

        Call it after toFront (true), which has already restored a minimised
        window and done the ordinary reordering. Harmless off Windows.
    */
    RaiseResult bringToForeground (juce::Component& desktopWindow);
}
