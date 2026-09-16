#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Where the destructive button goes in a confirmation dialog, per platform.
//
// WHY THIS IS A DECISION AND NOT A LAYOUT PREFERENCE
//
// The platforms disagree about what a dismissed dialog reports, and they
// disagree in opposite directions, so there is no single button order that is
// safe on both:
//
//   Windows  button ids are 0..N-1 in order. Escape is disabled, because
//            juce::NativeMessageBox does not pass TDF_ALLOW_DIALOG_CANCELLATION,
//            and a TaskDialogIndirect failure leaves the result at 0. So index 0
//            is what you get when something goes wrong, and the destructive
//            button must not be index 0.
//
//   Linux    the raw AlertWindow result is remapped (raw + N - 1) % N, the
//            LookAndFeel binds Escape to the SECOND button, and
//            userTriedToCloseWindow() exits with raw 0. Both of those resolve to
//            the LAST index, so the destructive button must not be last.
//
// "Cancel first, because Escape is harmless" is therefore wrong on Windows, and
// "Cancel last" is wrong on Linux. Each platform gets the order that keeps the
// destructive action off its own failure index.
//
// WHY IT LIVES IN A HEADER OF ITS OWN
//
// Because it was unverifiable where it was. The rule sat inline in a function
// that needs a plugin chain, a settings file, a state vault and a tray icon to
// reach, so confirming it on Linux or macOS meant a person sitting at one of
// those machines clicking a dialog -- which is why BACKLOG.md carried it as "not
// yet verified" rather than as done. As a pure function it is checked on all
// three platforms by CI, on every push.
//
// The index is DERIVED from the order rather than written down twice. That is
// the part that would rot: someone reorders the buttons for visual reasons and
// the hard-coded index now points at the other one, which is a dialog whose
// Cancel deletes your presets.
//==============================================================================
namespace lighthost::confirm
{
    /** Which platform rule applies. Passed in rather than read from the
        preprocessor so both branches are testable from one build.
    */
    enum class Platform
    {
        windowsLike,   ///< Windows and macOS: dismissal and failure report index 0
        linuxLike      ///< Linux and BSD: dismissal resolves to the last index
    };

    /** The platform this build is for. */
    [[nodiscard]] inline constexpr Platform thisPlatform()
    {
        #if JUCE_LINUX || JUCE_BSD
        return Platform::linuxLike;
        #else
        return Platform::windowsLike;
        #endif
    }

    /** Button labels, left to right, for a two-button confirmation.

        `destructive` is the one that does the irreversible thing.
    */
    [[nodiscard]] inline juce::StringArray buttonOrder (Platform platform,
                                                        const juce::String& destructive,
                                                        const juce::String& safe)
    {
        return platform == Platform::linuxLike
                   ? juce::StringArray { destructive, safe }
                   : juce::StringArray { safe, destructive };
    }

    /** The result index that means "do the destructive thing".

        Derived from the order, so reordering the buttons cannot leave this
        pointing at the wrong one.
    */
    [[nodiscard]] inline int destructiveIndex (const juce::StringArray& order,
                                               const juce::String& destructive)
    {
        return order.indexOf (destructive);
    }

    /** The index a dismissal or a failure reports on this platform.

        Exists so the rule can be asserted directly: the destructive button must
        never sit here.
    */
    [[nodiscard]] inline int dismissalIndex (Platform platform, int numButtons)
    {
        return platform == Platform::linuxLike ? numButtons - 1 : 0;
    }
}
