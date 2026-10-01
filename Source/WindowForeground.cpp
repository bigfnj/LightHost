#include "WindowForeground.hpp"

// After the JUCE include, never before. juce_BasicNativeHeaders.h has already
// set NOMINMAX, STRICT, WIN32_LEAN_AND_MEAN and the SDK version macros by this
// point, so including <windows.h> bare picks them up. Same arrangement, and
// the same reason, as ProcessQoS.cpp and IconMenu.cpp.
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace lighthost::window
{

juce::String RaiseResult::describe() const
{
    if (! attempted)
        return "not attempted";

    if (foreground)
        return "foreground";

    return "refused (" + juce::String (errorCode) + "), "
         + (liftedOnly ? juce::String ("lifted on top without focus")
                       : juce::String ("left where it was"));
}

RaiseResult bringToForeground (juce::Component& desktopWindow)
{
    RaiseResult result;

   #if JUCE_WINDOWS
    auto* peer = desktopWindow.getPeer();

    if (peer == nullptr)
        return result;

    auto* hwnd = static_cast<HWND> (peer->getNativeHandle());

    if (hwnd == nullptr)
        return result;

    result.attempted = true;

    if (SetForegroundWindow (hwnd) != 0)
    {
        result.foreground = true;
        return result;
    }

    // SetForegroundWindow documents no last-error contract, so this is
    // recorded for the log rather than interpreted.
    result.errorCode = static_cast<juce::uint32> (GetLastError());

    // Refused, and Windows has flashed the taskbar button. Any process may
    // toggle the topmost bit on its own window, and doing so places it above
    // the active window without changing who has the focus. Not the same as
    // being in front, but visible, which is the half the user can act on.
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

    result.liftedOnly = SetWindowPos (hwnd, HWND_TOPMOST,   0, 0, 0, 0, flags) != 0
                     && SetWindowPos (hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags) != 0;
   #else
    juce::ignoreUnused (desktopWindow);
   #endif

    return result;
}

}
