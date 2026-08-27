#pragma once

#include "UiMetrics.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// Light Host's own look and feel.
//
// Every window in this app was drawn by juce::LookAndFeel_V4, JUCE's default
// scheme since 2017. Nothing was broken about it; it simply looked like the
// framework default, because it was one, and the "Available Plugins" window
// inherits its table chrome from it wholesale.
//
// This subclass changes two things and deliberately nothing else:
//
//   - The palette. One accent colour, and a coherent set of greys, applied
//     through V4's ColourScheme plus the component colour ids V4 does not
//     derive from it. The accent is the blue already used for the bypass tick
//     in PreferencesWindow, so the app gains a single accent rather than a
//     second one.
//
//   - Buttons and table headers. Both get a rounded fill with distinct hover
//     and pressed states, which is what makes a control look like something
//     that can be pressed. The table header is where the plugin list looked
//     most obviously untouched.
//
// What it deliberately does NOT do, because Light Host is tray-resident and
// costs nothing while idle: no timers, no animation, no transparency, no
// gradients to re-rasterise and no images. Every method below is flat-fill
// paint-time work, so the idle profile stays exactly where it is.
//
// Two limits worth knowing, both inside juce::PluginListComponent and neither
// reachable from here:
//
//   - Its rows are a single flat fill of ListBox::backgroundColourId, so zebra
//     striping is not available through colours. It needs the component
//     replaced, which is a much larger job than this file.
//   - It hard-codes setRowHeight(20)/setHeaderHeight(22) in its constructor.
//     Those are overridden via getTableListBox() where it is constructed, not
//     here, because a look and feel is never consulted about them.
//==============================================================================
namespace lighthost::ui
{

class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    // One accent for the whole application. 0xff4a9eff is what PreferencesWindow
    // already filled the bypass tick with; reusing it keeps that agreement.
    static constexpr juce::uint32 kAccent = 0xff4a9eff;

    static constexpr float kCornerRadius  = 4.0f;
    static constexpr int   kHeaderPadding = 8;

    LookAndFeel() : juce::LookAndFeel_V4 (scheme())
    {
        const juce::Colour accent  { kAccent };
        const juce::Colour window  { 0xff23272f };
        const juce::Colour widget  { 0xff2b3038 };
        const juce::Colour menu    { 0xff20242b };
        const juce::Colour outline { 0xff3a4048 };
        const juce::Colour text    { 0xffe4e7ec };

        setColour (juce::ResizableWindow::backgroundColourId,      window);
        setColour (juce::DocumentWindow::textColourId,             text);

        setColour (juce::Label::textColourId,                      text);

        setColour (juce::TextButton::buttonColourId,               widget);
        setColour (juce::TextButton::buttonOnColourId,             accent);
        setColour (juce::TextButton::textColourOffId,              text);
        setColour (juce::TextButton::textColourOnId,               juce::Colours::white);

        setColour (juce::ComboBox::backgroundColourId,             widget);
        setColour (juce::ComboBox::textColourId,                   text);
        setColour (juce::ComboBox::outlineColourId,                outline);
        setColour (juce::ComboBox::arrowColourId,                  text.withAlpha (0.70f));
        setColour (juce::ComboBox::buttonColourId,                 widget);

        setColour (juce::PopupMenu::backgroundColourId,            menu);
        setColour (juce::PopupMenu::textColourId,                  text);
        setColour (juce::PopupMenu::headerTextColourId,            text.withAlpha (0.60f));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId,       juce::Colours::white);

        setColour (juce::ListBox::backgroundColourId,              window);
        setColour (juce::ListBox::outlineColourId,                 outline);
        setColour (juce::ListBox::textColourId,                    text);

        setColour (juce::TableHeaderComponent::backgroundColourId, menu);
        setColour (juce::TableHeaderComponent::textColourId,       text.withAlpha (0.82f));
        setColour (juce::TableHeaderComponent::outlineColourId,    outline);
        setColour (juce::TableHeaderComponent::highlightColourId,  accent);

        setColour (juce::ScrollBar::thumbColourId,                 outline.brighter (0.35f));
        setColour (juce::ScrollBar::trackColourId,                 window);

        setColour (juce::ProgressBar::backgroundColourId,          widget);
        setColour (juce::ProgressBar::foregroundColourId,          accent);

        setColour (juce::TextEditor::backgroundColourId,           menu);
        setColour (juce::TextEditor::textColourId,                 text);
        setColour (juce::TextEditor::outlineColourId,              outline);
        setColour (juce::TextEditor::focusedOutlineColourId,       accent);
        setColour (juce::TextEditor::highlightColourId,            accent.withAlpha (0.35f));

        setColour (juce::ToggleButton::textColourId,               text);
        setColour (juce::ToggleButton::tickColourId,               accent);
        setColour (juce::ToggleButton::tickDisabledColourId,       text.withAlpha (0.30f));

        setColour (juce::AlertWindow::backgroundColourId,          window);
        setColour (juce::AlertWindow::textColourId,                text);
        setColour (juce::AlertWindow::outlineColourId,             outline);

        setColour (juce::TooltipWindow::backgroundColourId,        menu);
        setColour (juce::TooltipWindow::textColourId,              text);
        setColour (juce::TooltipWindow::outlineColourId,           outline);
    }

    //==========================================================================
    /** JUCE lays out its own dialogs, and the only say we get in how big their
        buttons are is here. This is what brings the Cancel button on the plugin
        scan window, and any AlertWindow, to the same height as the buttons this
        application lays out itself.
    */
    int getAlertWindowButtonHeight() override
    {
        return lighthost::ui::metrics::pushButtonHeight;
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        const auto height = juce::jlimit (11.0f, 15.0f,
                                          static_cast<float> (buttonHeight) * 0.5f);
        return juce::Font (juce::FontOptions{}.withHeight (height));
    }

    /** A rounded fill with separate hover and pressed states. V4 draws a button
        as a squared-off fill that changes very little when pressed, which is the
        specific reason a button can look inert.
    */
    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);

        auto fill = backgroundColour;
        if (shouldDrawButtonAsDown)
            fill = fill.darker (0.30f);
        else if (shouldDrawButtonAsHighlighted)
            fill = fill.brighter (0.16f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, kCornerRadius);

        // The outline takes the accent while pressed, so the state stays legible
        // even where the fill change is subtle against a dark background.
        g.setColour (shouldDrawButtonAsDown
                         ? juce::Colour (kAccent).withAlpha (0.90f)
                         : getCurrentColourScheme()
                               .getUIColour (ColourScheme::outline)
                               .withAlpha (shouldDrawButtonAsHighlighted ? 0.90f : 0.55f));
        g.drawRoundedRectangle (bounds, kCornerRadius, 1.0f);
    }

    //==========================================================================
    void drawTableHeaderBackground (juce::Graphics& g,
                                    juce::TableHeaderComponent& header) override
    {
        auto bounds = header.getLocalBounds();

        g.setColour (findColour (juce::TableHeaderComponent::backgroundColourId));
        g.fillRect (bounds);

        // One rule under the whole header, rather than V4's full grid of 1px
        // dividers, is most of what stops a table reading as a framework default.
        g.setColour (findColour (juce::TableHeaderComponent::outlineColourId));
        g.fillRect (bounds.removeFromBottom (1));
    }

    void drawTableHeaderColumn (juce::Graphics& g,
                                juce::TableHeaderComponent&,
                                const juce::String& columnName,
                                int /*columnId*/,
                                int width, int height,
                                bool isMouseOver, bool isMouseDown,
                                int columnFlags) override
    {
        juce::Rectangle<int> area (width, height);
        const auto highlight = findColour (juce::TableHeaderComponent::highlightColourId);

        if (isMouseDown)
        {
            g.setColour (highlight.withAlpha (0.40f));
            g.fillRect (area);
        }
        else if (isMouseOver)
        {
            g.setColour (highlight.withAlpha (0.18f));
            g.fillRect (area);
        }

        // Inset divider on the trailing edge only, so columns read as grouped
        // rather than boxed in.
        g.setColour (findColour (juce::TableHeaderComponent::outlineColourId).withAlpha (0.60f));
        g.fillRect (area.getRight() - 1, 4, 1, juce::jmax (1, height - 8));

        area.reduce (kHeaderPadding, 0);

        const bool forwards  = (columnFlags & juce::TableHeaderComponent::sortedForwards)  != 0;
        const bool backwards = (columnFlags & juce::TableHeaderComponent::sortedBackwards) != 0;

        if (forwards || backwards)
        {
            const auto arrowArea = area.removeFromRight (14)
                                       .withSizeKeepingCentre (8, 5)
                                       .toFloat();
            juce::Path arrow;

            if (forwards)
                arrow.addTriangle (arrowArea.getX(),       arrowArea.getY(),
                                   arrowArea.getRight(),   arrowArea.getY(),
                                   arrowArea.getCentreX(), arrowArea.getBottom());
            else
                arrow.addTriangle (arrowArea.getX(),       arrowArea.getBottom(),
                                   arrowArea.getRight(),   arrowArea.getBottom(),
                                   arrowArea.getCentreX(), arrowArea.getY());

            g.setColour (highlight);
            g.fillPath (arrow);
        }

        g.setColour (findColour (juce::TableHeaderComponent::textColourId));
        g.setFont (juce::Font (juce::FontOptions{}.withHeight (12.5f)).boldened());
        g.drawFittedText (columnName, area, juce::Justification::centredLeft, 1);
    }

private:
    static ColourScheme scheme()
    {
        return {
            0xff23272f,   // windowBackground
            0xff2b3038,   // widgetBackground
            0xff20242b,   // menuBackground
            0xff3a4048,   // outline
            0xffe4e7ec,   // defaultText
            kAccent,      // defaultFill
            0xffffffff,   // highlightedText
            kAccent,      // highlightedFill
            0xffdde1e8    // menuText
        };
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LookAndFeel)
};

} // namespace lighthost::ui
