#pragma once

#include <juce_core/juce_core.h>

#include <optional>

//==============================================================================
// Noticing that the audio device in use is not the one that was asked for.
//
// WHY THIS EXISTS
//
// juce::AudioDeviceSetup identifies a device by its display NAME and nothing
// else. The persisted settings are a DEVICESETUP element carrying
// audioInputDeviceName and audioOutputDeviceName; there is no id anywhere in it.
// So renaming a Windows endpoint while it is selected leaves a stored name that
// matches no device on the system.
//
// JUCE handles that by falling back to the default device, which is reasonable.
// What is not reasonable is that it happens in silence, and the silence is not
// an oversight on our side -- it is unreachable code on ours. From
// AudioDeviceManager::initialiseFromXML:
//
//     error = setAudioDeviceSetup (setup, true);
//
//     if (error.isNotEmpty() && selectDefaultDeviceOnFailure)
//         error = initialise (..., nullptr, false, preferredDefaultDeviceName);
//
// setAudioDeviceSetup returns exactly the message wanted -- "No such device: X"
// -- and then the fallback's return value OVERWRITES it. The fallback succeeds,
// so the error we check is empty, every time. The host had a status report
// wired up for this and it could never fire.
//
// The cost of that was real. A processed microphone chain was routed to room
// speakers across three device-change cycles with nothing reported anywhere: not
// in the tray tooltip, not in the Preferences status row, not in the log.
//
// WHY THE MESSAGE CAN NAME THE MISSING DEVICE
//
// The fallback path calls setAudioDeviceSetup with treatAsChosenDevice = false,
// which does not call updateXml(). So the stored settings still hold the name
// the user actually chose, and comparing it against what is open tells us both
// halves: that a substitution happened, and what went missing.
//
// That window is not open forever, and this is the honest limit of the
// approach. autoMatchSampleRate, the Preferences Apply path, and any change made
// in the device panel -- including one as incidental as a buffer size -- all call
// setAudioDeviceSetup with treatAsChosenDevice = true, which calls updateXml()
// and adopts the FALLBACK device as the stored choice. Once that has happened
// the requested name is gone, the comparison finds nothing wrong, and a
// substitution that is still in force becomes undetectable.
//
// So the check runs at startup, on every device change, and before the
// Preferences window writes device state -- i.e. at every point where the
// evidence still exists. What it cannot do is notice a substitution after JUCE
// has overwritten the record of what was asked for. Reporting it while the
// evidence exists is a large improvement on never reporting it, and closing the
// remaining gap needs the request tracked separately from JUCE's own state.
//
// WHY COMPARISON IS FUZZY
//
// It matches JUCE's own deviceListContains, which compares trimmed and
// case-insensitively. Anything stricter would report a device as missing when
// only its capitalisation had changed, which is the sort of false alarm that
// teaches people to ignore a status row.
//==============================================================================
namespace lighthost::device
{
    /** True when two device names refer to the same device, by JUCE's rule. */
    [[nodiscard]] inline bool namesMatch (const juce::String& a, const juce::String& b)
    {
        return a.trim().equalsIgnoreCase (b.trim());
    }

    /** One side of the setup: what was asked for, and what is actually open. */
    struct Requested
    {
        juce::String input;
        juce::String output;
    };

    struct Actual
    {
        juce::String input;
        juce::String output;
    };

    /** Describes a substituted device, or nothing when the setup is as asked.

        An empty requested name is not a request, so it cannot be substituted --
        that is an input-only or output-only configuration, not a fault. An empty
        ACTUAL name against a non-empty request is a substitution: the device was
        asked for and nothing is open in its place.
    */
    [[nodiscard]] inline std::optional<juce::String> describeSubstitution (const Requested& requested,
                                                                          const Actual& actual)
    {
        const auto substituted = [] (const juce::String& want, const juce::String& got)
        {
            return want.isNotEmpty() && ! namesMatch (want, got);
        };

        const bool inputChanged  = substituted (requested.input,  actual.input);
        const bool outputChanged = substituted (requested.output, actual.output);

        if (! inputChanged && ! outputChanged)
            return {};

        // Named individually rather than as a count. The whole point is to say
        // which device went missing, because the user is the only one who can
        // decide whether the replacement is acceptable -- and when the output is
        // the one that moved, the replacement is very likely a speaker.
        const auto describe = [] (const juce::String& role,
                                  const juce::String& want,
                                  const juce::String& got)
        {
            return role + " \"" + want + "\" is not available; using "
                 + (got.isNotEmpty() ? "\"" + got + "\"" : juce::String ("nothing"));
        };

        juce::StringArray parts;

        if (inputChanged)
            parts.add (describe ("Input", requested.input, actual.input));

        if (outputChanged)
            parts.add (describe ("Output", requested.output, actual.output));

        return parts.joinIntoString (". ") + ". Re-select it in Preferences if this is wrong.";
    }

    /** Pulls the requested names out of a persisted DEVICESETUP element.

        Returns empty names when there is no stored state, which correctly reads
        as "nothing was requested" on a first run.

        The legacy single-name attribute is handled the same way JUCE handles it,
        because a settings file written by an older build still uses it.
    */
    [[nodiscard]] inline Requested requestedFrom (const juce::XmlElement* deviceSetup)
    {
        if (deviceSetup == nullptr)
            return {};

        const auto legacy = deviceSetup->getStringAttribute ("audioDeviceName");

        if (legacy.isNotEmpty())
            return { legacy, legacy };

        return { deviceSetup->getStringAttribute ("audioInputDeviceName"),
                 deviceSetup->getStringAttribute ("audioOutputDeviceName") };
    }
}
