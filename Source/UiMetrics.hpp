#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// The few numbers that decide whether two controls look like the same kind of
// control.
//
// This exists because they were scattered. Five push buttons had four different
// heights: Apply 28, "+ Add Plugin" 22, "Show Log" 22, "Get VB-CABLE" 22, and the
// plugin list Options button 30. Not one of those heights was chosen — every one
// fell out of an unrelated row height and a vertical inset. Six pixels is the
// difference between reading as a button and reading as a caption, so the height
// is stated once, here, and every layout site asks for it.
//
// Deliberately not in LookAndFeel.hpp. A look and feel decides how a control is
// painted; these decide how much room it is given, and the two are consulted by
// different code at different times. The one exception is JUCE's own dialogs:
// their button height is only reachable through the look and feel, so
// LookAndFeel.hpp overrides getAlertWindowButtonHeight to return the value below.
// That is what brings the Cancel button on the plugin scan window into line with
// the buttons this application lays out itself.
//==============================================================================
namespace lighthost::ui::metrics
{
    /** The height every push button in this application is laid out at. */
    inline constexpr int pushButtonHeight = 28;

    /** Vertical padding around a push button that owns its own strip, rather than
        sharing a row with a label.
    */
    inline constexpr int pushButtonPadY = 3;

    /** The bounds a push button of this width should occupy inside `strip`: the
        shared height, centred vertically, and never wider or taller than the room
        actually available.

        Callers still decide where the strip comes from. A button pinned to the
        trailing edge of a row and one sitting at the bottom left of a table are
        positioned differently; what they must agree on is size.
    */
    [[nodiscard]] inline juce::Rectangle<int> pushButton (juce::Rectangle<int> strip,
                                                          int width)
    {
        return strip.withSizeKeepingCentre (juce::jmin (width, strip.getWidth()),
                                            juce::jmin (pushButtonHeight, strip.getHeight()));
    }

    /** The height a strip needs to hold a push button with its padding. */
    [[nodiscard]] inline constexpr int pushButtonStripHeight()
    {
        return pushButtonHeight + pushButtonPadY * 2;
    }
}
