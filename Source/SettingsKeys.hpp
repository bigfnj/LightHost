#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// The settings-file keys that more than one file has to agree on.
//
// This exists for the same reason NodeIds.hpp and Lanes.hpp do: a name written
// out by hand in several places is a name that can drift, and these three had
// spread to eight sites across five files with nothing holding them together.
//
// What a drift costs here is specific, and worse than a compile error:
//
//   "audioDeviceState"  is read by the device-substitution check and written by
//                       two other paths. If the reader and the writers stop
//                       agreeing, the check compares the open devices against a
//                       stored request that is not there, finds nothing wrong,
//                       and reports nothing -- for ever. The feature 5.2.0 is
//                       named for would fail silently, which is the exact
//                       failure it was built to end.
//
//   "pluginList"        is the scanned plugin list, and
//   "pluginListActive"  is the chain. --render reads both. If the renderer's
//                       spelling drifts from IconMenu's, it renders an empty
//                       chain and reports success, and the offline renderer
//                       exists precisely to produce numbers that can be
//                       trusted. 5.1.0 has a whole entry about five ways it
//                       used to report success it had not earned.
//
// In every case the failure is a silent wrong answer rather than a crash, so
// nothing would catch it. One owner, and the question cannot be asked twice.
//
// THESE ARE AN ON-DISK CONTRACT. They are the names inside a settings file
// written by every released version, so they cannot be renamed for tidiness --
// only migrated. Tests/SettingsKeysTests.cpp pins the literal strings for that
// reason: the test exists to fail if someone "improves" a name.
//
// The per-plugin chain keys are deliberately NOT here. They are built rather
// than spelled -- chain::Store::keyFor composes them from a plugin identity --
// so Store already owns them, and DECISIONS.md records why they sit outside it.
//==============================================================================
namespace lighthost::keys
{
    /** Every plugin the scanner has found, as KnownPluginList XML. */
    inline constexpr const char* pluginList = "pluginList";

    /** The plugins in the chain, as KnownPluginList XML. Membership only;
        order, lane, bypass and state live in the chain::Store keys.
    */
    inline constexpr const char* pluginListActive = "pluginListActive";

    /** The audio device setup, as JUCE's DEVICESETUP XML.

        Both a record of what is open and, crucially, a record of what was
        ASKED for: JUCE's fallback path does not rewrite it, which is what lets
        a substituted device be noticed at all.
    */
    inline constexpr const char* audioDeviceState = "audioDeviceState";

    /** The device names the user chose, as opposed to the ones JUCE settled on.

        Written only when a choice is made in Preferences, and never by the
        sample-rate correction or by any other incidental device write, so it
        survives the writes that erase the request from DEVICESETUP.
    */
    inline constexpr const char* requestedDevices = "requestedAudioDevices";
}
