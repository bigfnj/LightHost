#pragma once

#include <juce_core/juce_core.h>

#include <memory>
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
// WHY THE REQUEST IS ALSO RECORDED SEPARATELY
//
// That window used to close, and then the check was dead. Three sites here pass
// treatAsChosenDevice = true: autoMatchSampleRate, and both of the Preferences
// Apply writes. After any of them the requested name is gone from DEVICESETUP,
// the comparison finds nothing wrong, and a substitution that is still in force
// is undetectable for good. 5.2.0 shipped with that limit documented rather
// than fixed.
//
// Counting only our own call sites understates it. updateXml() rewrites the
// whole element from the devices currently open, and JUCE calls it from
// setMidiInputDeviceEnabled and setDefaultMidiOutputDevice too
// (juce_AudioDeviceManager.cpp:1202, :1289) -- neither of which touches an
// audio device. Light Host calls neither today, so that pair is a trap rather
// than a bug: adding a MIDI input toggle would quietly erase the audio request
// as a side effect, with nothing to connect the two.
//
// So the names the user chose are kept in a key of our own,
// lighthost::keys::requestedDevices, written only by IconMenu's
// recordRequestedDevices and only from a deliberate choice. Nothing incidental
// writes it: autoMatchSampleRate moves the sample rate and leaves the record
// alone, which is the entire point of the record existing.
//
// Deciding what counts as a deliberate choice sounds like the hard part and is
// not. There is no juce::AudioDeviceSelectorComponent anywhere in this
// application -- the device panel is Light Host's own combo boxes, and
// Preferences Apply already snapshots the chosen names before it applies them.
// A deliberate choice is therefore exactly "a name that came out of those
// combos", and there is no incidental device edit to tell it apart from.
//
// DEVICESETUP is still the source when no record exists: a first run, and an
// install upgrading from 5.2.0 that will not have a record until the next
// Apply. See requestToCompare.
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

    //==========================================================================
    // The request we record ourselves, which the writes above cannot erase.

    /** True when a device combo holds a device name rather than a placeholder.

        The Preferences combos show "(no input devices)" when a list is empty,
        and the Apply path already refuses to select anything starting with "("
        for that reason. Recording one as a request would ask for a device that
        cannot exist, and report a substitution on every launch afterwards --
        the false alarm the fuzzy comparison above exists to avoid, arriving
        from the other end.
    */
    [[nodiscard]] inline bool isDeviceChoice (const juce::String& name)
    {
        const auto trimmed = name.trim();

        return trimmed.isNotEmpty() && ! trimmed.startsWith ("(");
    }

    /** A request built from two combo entries, with placeholders dropped.

        Trimmed on the way in. namesMatch trims on comparison anyway, so this
        buys nothing for the check itself -- it is for the settings file, which
        is plain XML that people do read and edit.
    */
    [[nodiscard]] inline Requested asRequest (const juce::String& input,
                                              const juce::String& output)
    {
        return { isDeviceChoice (input)  ? input.trim()  : juce::String(),
                 isDeviceChoice (output) ? output.trim() : juce::String() };
    }

    /** The element a recorded request is stored as.

        Deliberately not DEVICESETUP. The two elements mean different things --
        one is what was asked for, the other is what JUCE last settled on -- and
        a reader that took one for the other would be comparing JUCE's state
        against itself, which is the silent no-op this whole file exists to
        stop.
    */
    inline constexpr const char* requestedElementName = "REQUESTEDDEVICES";

    /** Encodes a request for the settings file.

        XML rather than a delimited pair because a device name is arbitrary
        text: "Speakers (Realtek(R) Audio)" is an ordinary one, and any
        separator character picked would eventually turn up inside a name and
        split it in half. It is also how audioDeviceState is already stored, so
        the read side is the same getXmlValue call.
    */
    [[nodiscard]] inline std::unique_ptr<juce::XmlElement> encodeRequested (const Requested& requested)
    {
        auto xml = std::make_unique<juce::XmlElement> (requestedElementName);
        xml->setAttribute ("input",  requested.input);
        xml->setAttribute ("output", requested.output);

        return xml;
    }

    /** True when a recorded request exists at all.

        Separate from whether it names anything: a record holding two empty
        names is a user who chose nothing, and must not fall back to
        DEVICESETUP, or an explicit "no device" would resurrect the stale
        request it replaced.
    */
    [[nodiscard]] inline bool hasRecord (const juce::XmlElement* recorded)
    {
        return recorded != nullptr && recorded->hasTagName (requestedElementName);
    }

    /** Reads back what encodeRequested wrote.

        A null element, or one written by something else, reads as two empty
        names -- which describeSubstitution treats as no request, so a corrupt
        or foreign value makes the check quiet rather than noisy.
    */
    [[nodiscard]] inline Requested decodeRequested (const juce::XmlElement* recorded)
    {
        if (! hasRecord (recorded))
            return {};

        return { recorded->getStringAttribute ("input"),
                 recorded->getStringAttribute ("output") };
    }

    /** Which request the substitution check should compare the open devices
        against.

        The recorded one whenever there is one, because it is the only copy that
        survives updateXml(). DEVICESETUP otherwise, and that fallback is not a
        corner case: a first run has neither, and an install upgrading from
        5.2.0 has a DEVICESETUP written by the old build and no record until the
        next time Preferences is applied. Ignoring DEVICESETUP would switch the
        check off for those users until they happened to press Apply.

        Both absent gives two empty names, so a first run reports nothing.
    */
    [[nodiscard]] inline Requested requestToCompare (const juce::XmlElement* recorded,
                                                     const juce::XmlElement* deviceSetup)
    {
        if (hasRecord (recorded))
            return decodeRequested (recorded);

        return requestedFrom (deviceSetup);
    }

    //==========================================================================
    /** The "a person picked a device" flag.

        A type rather than a bare bool because the whole of the 5.3.0 device fix
        is a two-rule state machine, and a bare bool leaves both rules in prose.
        ONE verb sets it and ONE clears it, and the clearing verb names its
        precondition in its own signature.

        What goes wrong without the rules: recording unconditionally inverts the
        feature. Startup reports a substitution, the user opens Preferences to
        look at it, presses Apply for any reason at all, and the SUBSTITUTE is
        written down as the thing they asked for -- after which nothing can ever
        notice it again.
    */
    class DeviceChoiceFlag
    {
    public:
        /** Called ONLY from a device combo's onChange.

            Nothing else can: there is no other mutator and the member is
            private. rebuildDeviceCombos populates the combos with
            dontSendNotification, so the host cannot set this on the user's
            behalf -- a claim about JUCE, and therefore asserted rather than
            assumed, in DevicePolicyTests.
        */
        void noteUserChoice() noexcept { userChose = true; }

        [[nodiscard]] bool isUserMade() const noexcept { return userChose; }

        /** Clears the flag if, and only if, the record was actually written.

            Returns what it did, so a caller cannot silently mean the other
            thing. An Apply that could not write the record must not forget that
            a choice was made, or the next launch reports the substituted device
            as the one the user asked for.
        */
        bool clearIfRecorded (bool recordWasWritten) noexcept
        {
            if (! recordWasWritten)
                return false;

            userChose = false;
            return true;
        }

    private:
        bool userChose = false;
    };

    /** Everything the Apply path needs to know to decide about devices. */
    struct ApplyInputs
    {
        juce::String      inputCombo;
        juce::String      outputCombo;
        juce::StringArray availableInputs;
        juce::StringArray availableOutputs;
        juce::String      openInput;    ///< what the setup already holds
        juce::String      openOutput;
        bool              userChose   = false;
        bool              hasRecorder = false;   ///< onDevicesChosenFn is wired
    };

    struct ApplyDecision
    {
        bool writeInputName  = false;
        bool writeOutputName = false;
        bool recordChoice    = false;   ///< write REQUESTEDDEVICES
        bool clearUserChoice = false;   ///< always equal to recordChoice
    };

    /** Whether a combo entry may be written into the device setup at all.

        The first two clauses are isDeviceChoice, which this used to spell out
        again at the Apply site. The third is the one that is specific to Apply:
        the input and output lists are unioned across every device type so they
        are not empty before the type is switched, so a name can be offered by
        the combo and not exist under the type being applied.
    */
    [[nodiscard]] inline bool acceptableForApply (const juce::String& name,
                                                  const juce::StringArray& available)
    {
        return isDeviceChoice (name) && available.contains (name);
    }

    /** What an Apply should do about the two device names.

        A name is written when the user chose it, OR when it is the device
        already open -- the second half matters because an Apply pressed for
        some other reason must not change the devices. Without it, a user who
        only ever picked an output had a microphone opened for them by an Apply
        they pressed to change a plugin.

        recordChoice and clearUserChoice are computed in ONE place and returned
        as two fields, so "the flag clears only after the record is written"
        stops being a comment and becomes something a test can hold to.
    */
    [[nodiscard]] inline ApplyDecision decideDeviceApply (const ApplyInputs& inputs)
    {
        ApplyDecision decision;

        decision.writeInputName = acceptableForApply (inputs.inputCombo, inputs.availableInputs)
                               && (inputs.userChose || inputs.inputCombo == inputs.openInput);

        decision.writeOutputName = acceptableForApply (inputs.outputCombo, inputs.availableOutputs)
                                && (inputs.userChose || inputs.outputCombo == inputs.openOutput);

        decision.recordChoice    = inputs.userChose && inputs.hasRecorder;
        decision.clearUserChoice = decision.recordChoice;

        return decision;
    }
}
