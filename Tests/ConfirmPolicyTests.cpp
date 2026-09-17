#include "../Source/ConfirmPolicy.hpp"

#include <juce_core/juce_core.h>

#include <optional>

//==============================================================================
// Where the destructive button goes in a confirmation dialog.
//
// This exists because the rule was previously unverifiable. It sat inline in
// IconMenu::confirmDeletePluginStates, which needs a plugin chain, a settings
// file, a state vault and a tray icon before it can be reached -- so checking it
// on Linux or macOS meant a person at one of those machines clicking a dialog.
// BACKLOG.md carried it as "not yet verified" for exactly that reason.
//
// Both platform rules are asserted from one build, because the Platform value is
// a parameter rather than a preprocessor branch. The consequence of getting it
// wrong is a dialog where dismissing it erases every saved plugin preset.
//==============================================================================
namespace
{
    using namespace lighthost::confirm;

    constexpr auto kDelete = "Delete";
    constexpr auto kCancel = "Cancel";
}

//==============================================================================
class ConfirmPolicyTests final : public juce::UnitTest
{
public:
    ConfirmPolicyTests()
        : juce::UnitTest ("Confirmation button order", "ConfirmPolicy") {}

    void runTest() override
    {
        beginTest ("the destructive button never sits on the dismissal index");
        {
            // The whole point, stated once for both platforms. Everything else
            // in this file is a consequence of this.
            for (const auto platform : { Platform::windowsLike, Platform::linuxLike })
            {
                const auto order = buttonOrder (platform, kDelete, kCancel);

                expectEquals (order.size(), 2);
                expectNotEquals (destructiveIndex (order, kDelete),
                                 dismissalIndex (platform, order.size()),
                                 "dismissing this dialog would perform the "
                                 "destructive action");
            }
        }

        beginTest ("Windows puts Cancel first, because index 0 is its failure result");
        {
            // Button ids are 0..N-1 in order, Escape is disabled, and a
            // TaskDialogIndirect failure leaves the result at 0.
            const auto order = buttonOrder (Platform::windowsLike, kDelete, kCancel);

            expectEquals (order[0], juce::String (kCancel));
            expectEquals (order[1], juce::String (kDelete));
            expectEquals (destructiveIndex (order, kDelete), 1);
            expectEquals (dismissalIndex (Platform::windowsLike, 2), 0);
        }

        beginTest ("Linux puts Delete first, because dismissal resolves to the last index");
        {
            // The raw AlertWindow result is remapped (raw + N - 1) % N, the
            // LookAndFeel binds Escape to the second button, and
            // userTriedToCloseWindow() exits with raw 0. Both land on the last.
            const auto order = buttonOrder (Platform::linuxLike, kDelete, kCancel);

            expectEquals (order[0], juce::String (kDelete));
            expectEquals (order[1], juce::String (kCancel));
            expectEquals (destructiveIndex (order, kDelete), 0);
            expectEquals (dismissalIndex (Platform::linuxLike, 2), 1);
        }

        beginTest ("the two platforms genuinely need different orders");
        {
            // If these ever agree, one of the rules above has been misread and
            // the single-order shortcut would be safe. They do not agree, which
            // is why this is a per-platform decision rather than a preference.
            const auto windowsOrder = buttonOrder (Platform::windowsLike, kDelete, kCancel);
            const auto linuxOrder   = buttonOrder (Platform::linuxLike,   kDelete, kCancel);

            expect (windowsOrder != linuxOrder);
            expectEquals (windowsOrder[0], linuxOrder[1]);
        }

        beginTest ("the index is derived, so reordering cannot desynchronise it");
        {
            // The failure this guards: someone swaps the labels for visual
            // reasons and a hard-coded index now points at the other button.
            const auto swapped = buttonOrder (Platform::windowsLike, kCancel, kDelete);

            expectEquals (swapped[0], juce::String (kDelete));
            expectEquals (destructiveIndex (swapped, kDelete), 0,
                          "the index must follow the labels, not be written twice");
        }

        beginTest ("the labels are carried through, not assumed");
        {
            // Nothing here should depend on the words "Delete" and "Cancel".
            const auto order = buttonOrder (Platform::windowsLike, "Erase everything", "Back out");

            expectEquals (order[0], juce::String ("Back out"));
            expectEquals (order[1], juce::String ("Erase everything"));
            expectEquals (destructiveIndex (order, "Erase everything"), 1);
        }

        beginTest ("a label that is not in the order reports no index");
        {
            const auto order = buttonOrder (Platform::windowsLike, kDelete, kCancel);

            expectEquals (destructiveIndex (order, "Nonexistent"), -1,
                          "-1 is what StringArray::indexOf reports, and the "
                          "caller compares against a real result, so a typo "
                          "declines rather than deletes");
        }

        beginTest ("the compiled-in rule matches the machine this is running on");
        {
            // The expectation is stated against a RUNTIME fact, not against a
            // second copy of the production condition. This used to read
            //
            //     #if JUCE_LINUX || JUCE_BSD
            //     expect (thisPlatform() == Platform::linuxLike);
            //
            // which is ConfirmPolicy.hpp:57-61's own #if compared against a
            // duplicate of itself: invert the production one and this inverts
            // with it, both stay green, and the dialog ships with Delete on the
            // Escape key. No input could have failed it.
            //
            // juce::SystemStats::getOperatingSystemType() answers at run time
            // out of whichever juce_SystemStats_<platform>.cpp the build
            // compiled, so editing our header cannot move it. It is not
            // independent of JUCE_LINUX all the way down -- nothing in one
            // process is -- but it is independent of the line under test,
            // which is the line that can be edited.
            //
            // BSD reports Linux: juce_core.cpp:251-259 compiles the Linux
            // SystemStats for it, and that returns SystemStats::Linux, which
            // is the same side of this decision JUCE_BSD puts it on.
            const auto os = juce::SystemStats::getOperatingSystemType();

            // Listed, not defaulted. A platform nobody has worked out the
            // dismissal rule for has to fail here rather than quietly inherit
            // whichever branch the #else happens to be.
            const auto expected = [os]() -> std::optional<Platform>
            {
                if ((os & juce::SystemStats::Windows) != 0) return Platform::windowsLike;
                if ((os & juce::SystemStats::MacOSX)  != 0) return Platform::windowsLike;
                if ((os & juce::SystemStats::Linux)   != 0) return Platform::linuxLike;
                return {};
            }();

            expect (expected.has_value(),
                    "running on " + juce::SystemStats::getOperatingSystemName()
                        + ", which is none of the three platforms Light Host ships "
                          "for, so nothing here knows which dismissal rule applies");

            if (expected.has_value())
                expect (thisPlatform() == *expected,
                        "the compiled-in platform rule disagrees with the machine "
                        "this is running on ("
                            + juce::SystemStats::getOperatingSystemName() + ")");

            // And whichever it is, the live order is the safe one. This is the
            // assertion that used to require a person at the machine.
            const auto live = buttonOrder (thisPlatform(), kDelete, kCancel);

            expectNotEquals (destructiveIndex (live, kDelete),
                             dismissalIndex (thisPlatform(), live.size()));
        }
    }
};

static ConfirmPolicyTests confirmPolicyTests;
