#pragma once

#include "UiMetrics.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <functional>

//==============================================================================
// How tall the Preferences panel needs to be, and who is told when that
// changes.
//
// WHY THIS IS A SEPARATE FILE
//
// The arithmetic was a hand-maintained sum inside PreferencesContentComponent,
// agreeing with a 140-line resized() only by care. Adding a row to one and not
// the other lays the Apply button on top of it, and it looks almost right. The
// height budget comment beside it had already drifted once by 34 pixels, by its
// own admission.
//
// THE DEFECT THIS EXISTS TO FIX
//
// Showing the status row grew the fixed height by a row and called resized() on
// the panel -- and resized() only divides up the height the panel ALREADY has.
// The chain viewport is the single elastic section, so it absorbed the extra
// row until it hit its floor, and after that the Apply button was laid out
// below the panel's bottom edge. Nothing re-ran the viewport, so the panel was
// never re-heighted and the viewport never learned it had more to scroll: the
// button could not be reached by scrolling either.
//
// That was reachable at the default window size. The panel wants 644 px in a
// shell about 619 px tall, so the chain viewport is already at its 80 px floor
// before a status message arrives -- and a status message is what the
// application shows when a plugin fails to load.
//
// The fix is an interface the owner cannot forget to wire, because the viewport
// installs the hook in its own constructor.
//==============================================================================
namespace lighthost::ui::prefs
{
    /** The size a viewport's content should be given: the width it may use, and
        the height it needs.

        THE SCROLLBAR'S WIDTH IS SUBTRACTED ONLY WHEN A BAR WILL ACTUALLY BE
        SHOWN. Three call sites computed this by hand and two of the three
        disagreed -- the chain list subtracted it unconditionally, which is
        invisible on an elastic-width list but moves every number in a column
        that pins them to the right edge.

        NO ZERO-WIDTH EARLY RETURN. Two of those callers skipped the whole
        calculation while the viewport had not been laid out yet, which left
        their content at its PREVIOUS height -- the wrong half to skip. The
        width is cosmetic until the first layout; the height is what decides how
        far the thing can scroll. The width floor below covers the degenerate
        case instead.

        Cannot oscillate: the height comes from the content and does not depend
        on the width handed back.
    */
    [[nodiscard]] inline juce::Point<int> contentSizeFor (const juce::Viewport& viewport,
                                                          int preferredContentHeight,
                                                          int minimumWidth = 1)
    {
        const bool needsScrollBar = preferredContentHeight > viewport.getHeight();

        const int width = viewport.getWidth()
                        - (needsScrollBar ? viewport.getScrollBarThickness() : 0);

        return { std::max (minimumWidth, width),
                 std::max (viewport.getHeight(), preferredContentHeight) };
    }

    //==========================================================================
    /** Which optional rows the layout is currently showing.

        Both appear and disappear at runtime, and both cost a row when they do:
        the status row when something has failed, and the Windows virtual-input
        hint when the selected input looks like a virtual cable.
    */
    struct OptionalRows
    {
        bool status           = false;
        bool virtualInputHint = false;
    };

    /** Every metric resized() and the height report share, so the two cannot
        disagree.

        Defaulted to the shipped values, and a parameter so a test can vary them
        and assert a relationship rather than a magic number.
    */
    struct Metrics
    {
        int pad       = 10;
        int sectionH  = 22;
        int rowH      = 28;
        int gap       = 6;
        int buttonH   = 40;
        int meterH    = lighthost::ui::metrics::meterHeight;
        int minChainH = 80;
    };

    /** One optional row costs its own height plus the gap under it. */
    [[nodiscard]] constexpr int optionalRowHeight (const Metrics& m) noexcept
    {
        return m.rowH + m.gap;
    }

    /** Height of everything except the chain viewport, which is the only
        section that stretches.

        The section breakdown is spelled out rather than folded into one
        constant, because the whole failure this file addresses is a row being
        added to the layout and not to the sum.
    */
    [[nodiscard]] constexpr int fixedLayoutHeight (const Metrics& m, OptionalRows rows) noexcept
    {
        const int statusH = rows.status           ? optionalRowHeight (m) : 0;
        const int hintH   = rows.virtualInputHint ? optionalRowHeight (m) : 0;

        const int aboveChain = m.pad
            + statusH                                    // status row, when something failed
            + m.sectionH + m.gap + m.rowH + m.gap        // INPUT
            + m.meterH + m.gap                           // input meter
            + hintH                                      // virtual-input hint, when shown
            + m.sectionH + m.gap;                        // AUDIO CHAIN label

        const int belowChain = m.gap + m.rowH + m.gap                     // Add Plugin row
            + m.sectionH + m.gap + m.rowH + m.gap                         // LANE TRIM
            + m.sectionH + m.gap + m.rowH + m.gap                         // OUTPUT
            + m.meterH + m.gap                                            // output meter
            + m.sectionH + m.gap                                          // DEVICE SETTINGS label
            + 5 * (m.rowH + m.gap)                                        // API, rate, buffer, latency, load
            + m.buttonH + m.pad;                                          // Apply

        return aboveChain + belowChain;
    }

    /** The shortest the panel can be laid out at without a section losing its
        height. */
    [[nodiscard]] constexpr int preferredHeight (const Metrics& m, OptionalRows rows) noexcept
    {
        return fixedLayoutHeight (m, rows) + m.minChainH;
    }

    /** How tall the chain viewport ends up in a panel of `panelHeight`. */
    [[nodiscard]] constexpr int chainViewportHeight (const Metrics& m,
                                                     OptionalRows rows,
                                                     int panelHeight) noexcept
    {
        return std::max (m.minChainH, panelHeight - fixedLayoutHeight (m, rows));
    }

    /** True when a panel of `panelHeight` has room for every section.

        The Apply button is the LAST thing resized() places, so "the layout
        fits" and "Apply is on screen" are the same statement -- which is why
        this is a predicate over the budget rather than a second walk of the
        stack. A second walk would be a second source of truth, and this layout
        already lost 34 pixels once to a comment that was one.
    */
    [[nodiscard]] constexpr bool layoutFits (const Metrics& m,
                                             OptionalRows rows,
                                             int panelHeight) noexcept
    {
        return panelHeight >= preferredHeight (m, rows);
    }

    //==========================================================================
    /** A panel whose height its owner has to ask for. */
    struct HeightReportingPanel
    {
        virtual ~HeightReportingPanel() = default;

        [[nodiscard]] virtual int getPreferredHeight() const = 0;

        /** Installed by the PanelViewport that owns this panel, and called by
            the panel whenever getPreferredHeight() would now answer
            differently.

            The panel cannot re-height itself: its bounds are the viewport's to
            set. Before this existed, an optional row appearing called resized()
            and nothing else, so the panel re-divided a height that had not
            grown.
        */
        std::function<void()> onPreferredHeightChanged;
    };

    /** Holds a panel and scrolls it when the window is shorter than its layout
        needs.

        The re-height hook is installed HERE, in the constructor, rather than by
        whoever builds the viewport. That is the point of the class: the wiring
        is the thing that was missing, so it must not be something a caller can
        forget.
    */
    template <typename PanelType>
    class PanelViewport final : public juce::Viewport
    {
    public:
        explicit PanelViewport (PanelType* panelToOwn)
            : panel (panelToOwn)
        {
            static_assert (std::is_base_of_v<juce::Component, PanelType>,
                           "the panel has to be a Component for the viewport to show it");
            static_assert (std::is_base_of_v<HeightReportingPanel, PanelType>,
                           "the panel has to report its own preferred height, or this "
                           "viewport cannot know when it has grown");

            setViewedComponent (panel, true);   // the viewport owns it
            setScrollBarsShown (true, false);

            if (panel != nullptr)
                panel->onPreferredHeightChanged = [this] { updatePanelSize(); };
        }

        void resized() override
        {
            juce::Viewport::resized();
            updatePanelSize();
        }

        /** Sizes the panel to the width it may use and the height its layout
            needs.

            Called on resize AND from the panel itself, because the height it
            needs changes when an optional row appears. Without the second entry
            point the panel is laid out inside its old height, and once the
            chain viewport is at its floor there is nowhere for the new row to
            come from.
        */
        void updatePanelSize()
        {
            if (panel == nullptr)
                return;

            const auto size = contentSizeFor (*this, panel->getPreferredHeight(), 200);
            panel->setSize (size.x, size.y);
        }

        PanelType* panel = nullptr;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanelViewport)
    };
}
