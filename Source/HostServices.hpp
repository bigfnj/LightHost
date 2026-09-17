#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

//==============================================================================
// The two things the application object lends to everything else.
//
// Both are defined in HostStartup.cpp, which owns the JUCEApplication, and both
// are needed well away from it: the settings file by almost everything, and the
// log path by the Preferences window's "Show Log" button.
//
// They used to be declared wherever they were wanted -- getAppProperties in
// IconMenu.hpp and again in PreferencesWindow.cpp, getLogFile only in
// PreferencesWindow.cpp and nowhere near its definition. Nothing checked the
// copies against the original, so a changed signature would have been a link
// error rather than a compile error, and Clang complained from the other end
// with -Wmissing-prototypes because the definition had no declaration in scope.
//
// One header, included by the definition as well as by the callers, so the
// compiler checks them against each other.
//==============================================================================

/** The user's settings file, as an ApplicationProperties. */
juce::ApplicationProperties& getAppProperties();

/** Where this run is logging.

    Follows the self-test's throwaway folder, so a self-test never points the
    user at the real log.
*/
juce::File getLogFile();
