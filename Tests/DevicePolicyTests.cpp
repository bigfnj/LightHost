#include "../Source/DevicePolicy.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// Noticing that the open audio device is not the one that was asked for.
//
// This is tested as a free function because the thing it replaces could not be
// tested at all. The host had a status report wired to the return value of
// AudioDeviceManager::initialise, and JUCE overwrites that value with the
// fallback's result -- so the check existed, looked correct, and was
// unreachable. A processed microphone chain went to room speakers across three
// device-change cycles and nothing was reported anywhere.
//
// The interesting cases are all about NOT crying wolf. A status row that reports
// a device as missing when only its capitalisation changed, or that repeats
// itself on an input-only configuration, is a status row people learn to ignore
// -- and then the one report that mattered goes unread too.
//==============================================================================
namespace
{
    using namespace lighthost::device;

    /** Parses a DEVICESETUP element as AudioDeviceManager would have written it. */
    std::unique_ptr<juce::XmlElement> setupXml (const juce::String& input,
                                                const juce::String& output)
    {
        auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
        xml->setAttribute ("deviceType", "Windows Audio");
        xml->setAttribute ("audioInputDeviceName",  input);
        xml->setAttribute ("audioOutputDeviceName", output);
        return xml;
    }
}

//==============================================================================
class DevicePolicyTests final : public juce::UnitTest
{
public:
    DevicePolicyTests()
        : juce::UnitTest ("Device substitution", "DevicePolicy") {}

    void runTest() override
    {
        beginTest ("the devices that were asked for report nothing");
        {
            const auto message = describeSubstitution ({ "Mic", "Speakers" },
                                                       { "Mic", "Speakers" });
            expect (! message.has_value());
        }

        beginTest ("a substituted output is reported, and names both devices");
        {
            // The case that cost an hour: a dock re-enumeration moved playback
            // off an unplugged headset, and the processed chain went to the room.
            const auto message = describeSubstitution ({ "Mic", "Mic Chain INPUT" },
                                                       { "Mic", "Speakers (Dock)" });

            expect (message.has_value(), "a different device was open and nothing was said");
            expect (message->contains ("Mic Chain INPUT"),
                    "the message must name the device that went missing");
            expect (message->contains ("Speakers (Dock)"),
                    "the message must name what is being used instead");
            expect (message->contains ("Output"), "and which side of the setup moved");
        }

        beginTest ("a substituted input is reported");
        {
            const auto message = describeSubstitution ({ "Microphone (USB audio CODEC)", "Out" },
                                                       { "J-Dizzle MiC (VB-Audio)",      "Out" });

            expect (message.has_value());
            expect (message->contains ("Input"));
            expect (message->contains ("Microphone (USB audio CODEC)"));
        }

        beginTest ("both sides substituted at once are reported together");
        {
            const auto message = describeSubstitution ({ "InA", "OutA" }, { "InB", "OutB" });

            expect (message.has_value());
            expect (message->contains ("InA"));
            expect (message->contains ("OutA"));
            expect (message->contains ("Input") && message->contains ("Output"),
                    "one report should not hide the other");
        }

        beginTest ("a rename that only changed case is not a substitution");
        {
            // Windows endpoint names round-trip through several layers. Reporting
            // this would be a false alarm, and false alarms are how a status row
            // stops being read.
            expect (! describeSubstitution ({ "mic chain input", "speakers" },
                                            { "Mic Chain INPUT", "Speakers" }).has_value());
        }

        beginTest ("surrounding whitespace is not a substitution either");
        {
            expect (! describeSubstitution ({ "  Mic  ", "Speakers" },
                                            { "Mic",     "Speakers  " }).has_value());
        }

        beginTest ("an empty request is not a request, so it cannot be substituted");
        {
            // An input-only or output-only configuration is a legitimate setup,
            // not a fault. Reporting it would fire on every launch of one.
            expect (! describeSubstitution ({ "", "Speakers" }, { "Some Input", "Speakers" })
                         .has_value(),
                    "no input was asked for, so whatever opened is not a substitution");

            expect (! describeSubstitution ({ "Mic", "" }, { "Mic", "Some Output" })
                         .has_value());

            expect (! describeSubstitution ({ "", "" }, { "Anything", "Anything" })
                         .has_value(),
                    "a first run with no stored settings must be silent");
        }

        beginTest ("a device asked for with nothing open IS a substitution");
        {
            const auto message = describeSubstitution ({ "Mic", "Speakers" }, { "Mic", "" });

            expect (message.has_value(),
                    "the output was requested and nothing is open in its place");
            expect (message->contains ("nothing"));
        }

        beginTest ("the message tells the user what to do about it");
        {
            const auto message = describeSubstitution ({ "A", "B" }, { "A", "C" });

            expect (message.has_value());
            expect (message->containsIgnoreCase ("Preferences"),
                    "a report with no remedy is only half a report");
        }

        //======================================================================
        beginTest ("the requested names come out of the stored settings");
        {
            const auto xml = setupXml ("Microphone (USB audio CODEC)",
                                       "Mic Chain INPUT (VB-Audio Virtual Cable)");
            const auto requested = requestedFrom (xml.get());

            expectEquals (requested.input,  juce::String ("Microphone (USB audio CODEC)"));
            expectEquals (requested.output, juce::String ("Mic Chain INPUT (VB-Audio Virtual Cable)"));
        }

        beginTest ("no stored settings means nothing was requested");
        {
            const auto requested = requestedFrom (nullptr);

            expect (requested.input.isEmpty());
            expect (requested.output.isEmpty());
        }

        beginTest ("the pre-JUCE-8 single-name attribute still reads");
        {
            // A settings file written by an older build uses one name for both
            // sides, and JUCE still honours it on read. Ignoring it here would
            // make every such install report a substitution on first launch.
            juce::XmlElement legacy ("DEVICESETUP");
            legacy.setAttribute ("audioDeviceName", "Shared Device");

            const auto requested = requestedFrom (&legacy);

            expectEquals (requested.input,  juce::String ("Shared Device"));
            expectEquals (requested.output, juce::String ("Shared Device"));
        }

        beginTest ("a stored setup naming no device requests nothing");
        {
            juce::XmlElement empty ("DEVICESETUP");
            empty.setAttribute ("deviceType", "Windows Audio");

            const auto requested = requestedFrom (&empty);

            expect (requested.input.isEmpty());
            expect (requested.output.isEmpty());
        }

        beginTest ("stored settings and an open device compose end to end");
        {
            const auto xml = setupXml ("Mic", "Mic Chain INPUT");

            expect (! describeSubstitution (requestedFrom (xml.get()),
                                            { "Mic", "Mic Chain INPUT" }).has_value());

            const auto moved = describeSubstitution (requestedFrom (xml.get()),
                                                     { "Mic", "Speakers (Plugable Audio)" });

            expect (moved.has_value());
            expect (moved->contains ("Mic Chain INPUT"));
        }

        beginTest ("namesMatch is the rule JUCE uses to look a device up");
        {
            expect (namesMatch ("Speakers", "speakers"));
            expect (namesMatch (" Speakers ", "Speakers"));
            expect (! namesMatch ("Speakers", "Speakers (2)"),
                    "JUCE appends a suffix to duplicate endpoint names, and those "
                    "are genuinely different devices");
            expect (namesMatch ("", ""));
        }

        //======================================================================
        // The request recorded separately from JUCE's own state.
        //
        // DEVICESETUP stops being evidence of a REQUEST the moment anything
        // calls updateXml(), which three sites in the application do. These
        // cover the key that outlives those writes, and -- just as important --
        // the cases where it must NOT take over from DEVICESETUP.

        beginTest ("a recorded request survives the settings file it is stored in");
        {
            // Through a real PropertySet, because that is the path in
            // production: setValue serialises the element to text and
            // getXmlValue reparses it. Device names are arbitrary text, so the
            // attribute escaping has to hold or a name comes back truncated and
            // every launch reports a substitution that did not happen.
            juce::PropertySet settings;

            // Spelled with charToString so this source carries no escape of its
            // own to get wrong.
            const auto quote = juce::String::charToString ('"');
            const Requested chosen { "Mic & <Line In> " + quote + "2" + quote,
                                     "Line In (2- M-Audio M-Track)" };

            settings.setValue ("requestedAudioDevices", encodeRequested (chosen).get());

            const auto reread = decodeRequested (settings.getXmlValue ("requestedAudioDevices").get());

            expectEquals (reread.input,  chosen.input);
            expectEquals (reread.output, chosen.output);
        }

        beginTest ("no record at all reads as nothing requested");
        {
            expect (! hasRecord (nullptr));

            const auto none = decodeRequested (nullptr);

            expect (none.input.isEmpty());
            expect (none.output.isEmpty());
        }

        beginTest ("an element written by something else is not a record");
        {
            // A DEVICESETUP is the element most likely to arrive here by
            // mistake, and it carries the names under different attributes.
            // Accepting one would compare the open devices against two empty
            // strings and report nothing, for ever.
            const auto foreign = setupXml ("Mic", "Speakers");

            expect (! hasRecord (foreign.get()));

            const auto decoded = decodeRequested (foreign.get());

            expect (decoded.input.isEmpty());
            expect (decoded.output.isEmpty());
        }

        beginTest ("one side chosen and the other not is recorded as exactly that");
        {
            const auto encoded = encodeRequested ({ {}, "Speakers" });
            const auto decoded = decodeRequested (encoded.get());

            expect (decoded.input.isEmpty(), "no input was chosen, so none is requested");
            expectEquals (decoded.output, juce::String ("Speakers"));

            expect (! describeSubstitution (decoded, { "Whatever Opened", "Speakers" }).has_value(),
                    "an input that was never requested cannot have been substituted");
        }

        beginTest ("a combo placeholder is not a choice");
        {
            // The device combos show "(no input devices)" when a list is empty.
            // Recording one would ask for a device that cannot exist, and
            // report a substitution on every launch from then on.
            expect (! isDeviceChoice ("(no input devices)"));
            expect (! isDeviceChoice ("(no device)"));
            expect (! isDeviceChoice (""));
            expect (! isDeviceChoice ("   "));
            expect (isDeviceChoice ("Speakers (Realtek(R) Audio)"),
                    "a name that merely contains brackets is a real device");

            const auto request = asRequest ("(no input devices)", "Speakers");

            expect (request.input.isEmpty());
            expectEquals (request.output, juce::String ("Speakers"));
        }

        beginTest ("surrounding whitespace is trimmed before it is recorded");
        {
            const auto request = asRequest ("   Mic   ", "  Speakers  ");

            expectEquals (request.input,  juce::String ("Mic"));
            expectEquals (request.output, juce::String ("Speakers"));

            expect (! isDeviceChoice ("  (no device)  "),
                    "a padded placeholder is still a placeholder");

            expect (asRequest ("   ", "\t").input.isEmpty(),
                    "whitespace is not a device name");
        }

        beginTest ("a recorded request beats the stored DEVICESETUP");
        {
            // The case the record exists for. JUCE has already adopted the
            // fallback into DEVICESETUP -- one autoMatchSampleRate correction is
            // enough -- so DEVICESETUP now agrees with what is open and has
            // nothing to report. The record still holds what was asked for.
            const auto adopted  = setupXml ("Mic", "Speakers (Dock)");
            const auto recorded = encodeRequested ({ "Mic", "Mic Chain INPUT" });

            expect (! describeSubstitution (requestedFrom (adopted.get()),
                                            { "Mic", "Speakers (Dock)" }).has_value(),
                    "DEVICESETUP on its own is exactly the blind spot being closed");

            const auto message = describeSubstitution (
                requestToCompare (recorded.get(), adopted.get()),
                { "Mic", "Speakers (Dock)" });

            expect (message.has_value(), "the recorded request has to win");
            expect (message->contains ("Mic Chain INPUT"),
                    "and it is the recorded name that must be reported missing");
        }

        beginTest ("with no record the check falls back to DEVICESETUP");
        {
            // 5.2.0 wrote no record, so an install upgrading from it has only
            // this until the next Apply. Ignoring DEVICESETUP would switch the
            // check off for those users in the meantime.
            const auto stored = setupXml ("Mic", "Mic Chain INPUT");

            const auto message = describeSubstitution (requestToCompare (nullptr, stored.get()),
                                                       { "Mic", "Speakers" });

            expect (message.has_value());
            expect (message->contains ("Mic Chain INPUT"));
        }

        beginTest ("a record naming nothing does not fall back");
        {
            // Both combos on a placeholder is still a choice. Falling back here
            // would resurrect the stale DEVICESETUP request that choice
            // replaced, and report a substitution against a device the user has
            // already stopped asking for.
            const auto stored   = setupXml ("Old Mic", "Old Speakers");
            const auto recorded = encodeRequested (asRequest ("(no input devices)",
                                                              "(no output devices)"));

            expect (hasRecord (recorded.get()), "an empty record is still a record");

            const auto request = requestToCompare (recorded.get(), stored.get());

            expect (request.input.isEmpty());
            expect (request.output.isEmpty());
            expect (! describeSubstitution (request, { "Something", "Else" }).has_value());
        }

        beginTest ("a first run has neither, and says nothing");
        {
            const auto request = requestToCompare (nullptr, nullptr);

            expect (request.input.isEmpty());
            expect (request.output.isEmpty());
            expect (! describeSubstitution (request, { "Default In", "Default Out" }).has_value(),
                    "nothing was asked for, so nothing can have been substituted");
        }

        beginTest ("recording then re-reading reports the device that went missing");
        {
            // End to end over the storage the application actually uses: record
            // what the combos held, lose the device, and check the report names
            // it. Everything between is the same code IconMenu runs.
            juce::PropertySet settings;

            settings.setValue ("requestedAudioDevices",
                               encodeRequested (asRequest ("Microphone (USB audio CODEC)",
                                                           "  Mic Chain INPUT  ")).get());

            const auto recorded = settings.getXmlValue ("requestedAudioDevices");

            const auto message = describeSubstitution (
                requestToCompare (recorded.get(), nullptr),
                { "Microphone (USB audio CODEC)", "Speakers (Dock)" });

            expect (message.has_value());
            expect (message->contains ("Mic Chain INPUT"));
            expect (message->contains ("Speakers (Dock)"));
            expect (message->contains ("Output"));
        }
    }
};

static DevicePolicyTests devicePolicyTests;
