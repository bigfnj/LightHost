#include "../Source/PreferencesLayout.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// The Preferences height budget, and the re-height path an optional row needs.
//
// Two things are load-bearing here and neither was asserted before.
//
// The first is that the arithmetic and the layout agree. resized() lays out a
// fixed stack with removeFromTop and the height report is a separate sum; a row
// added to one and not the other puts the Apply button underneath it, and it
// looks almost right. The comment that used to hold this budget had already
// drifted by 34 pixels.
//
// The second is the defect itself. Showing the status row grew the height the
// panel needed, and nothing re-heighted the panel -- so once the chain viewport
// was at its floor the Apply button was laid out below the bottom edge, with
// the viewport unaware it had anything more to scroll to. That is reachable at
// the default window size, and the trigger is a plugin failing to load.
//==============================================================================
namespace
{
    using namespace lighthost::ui::prefs;

    /** A panel whose preferred height a test can move. */
    class FakePanel final : public juce::Component,
                            public HeightReportingPanel
    {
    public:
        [[nodiscard]] int getPreferredHeight() const override { return preferred; }

        void setPreferredHeight (int h) { preferred = h; }

    private:
        int preferred = 100;
    };

    //==========================================================================
    class PreferencesLayoutArithmeticTests final : public juce::UnitTest
    {
    public:
        PreferencesLayoutArithmeticTests()
            : juce::UnitTest ("Preferences height budget", "PreferencesLayout") {}

        void runTest() override
        {
            constexpr Metrics m {};

            beginTest ("showing the status row costs exactly one row");
            {
                // FAILS IF: a row is added to resized() and not to the sum,
                // which is how 34 pixels went missing from the comment this
                // replaced.
                const auto without = fixedLayoutHeight (m, {});
                const auto with    = fixedLayoutHeight (m, { true, false });

                expectEquals (with - without, optionalRowHeight (m),
                              "the status row does not cost exactly one row plus its gap");
            }

            beginTest ("the virtual-input hint costs the same, and the two add up");
            {
                const auto none  = fixedLayoutHeight (m, {});
                const auto hint  = fixedLayoutHeight (m, { false, true });
                const auto both  = fixedLayoutHeight (m, { true, true });

                expectEquals (hint - none, optionalRowHeight (m));
                expectEquals (both - none, 2 * optionalRowHeight (m),
                              "the two optional rows do not cost two rows together");
            }

            beginTest ("the chain viewport never goes below its floor");
            {
                for (int panelHeight = 0; panelHeight < preferredHeight (m, {}) * 2; panelHeight += 17)
                    expect (chainViewportHeight (m, {}, panelHeight) >= m.minChainH,
                            "the chain viewport was squeezed below the height it is "
                            "guaranteed, so the list has no rows to show");
            }

            beginTest ("at the chain viewport's floor, a new row cannot be absorbed");
            {
                // THE DEFECT. At exactly the preferred height with no optional
                // rows, the chain viewport is already at kMinChainH -- so there
                // is nothing left for a status row to come out of, and the
                // Apply button is laid out below the bottom edge.
                const auto atFloor = preferredHeight (m, {});

                expect (layoutFits (m, {}, atFloor),
                        "the panel does not fit at its own preferred height");

                expect (! layoutFits (m, { true, false }, atFloor),
                        "the panel claims it can absorb a status row at the height where "
                        "the chain viewport is already at its minimum -- which is the "
                        "height at which the Apply button used to disappear");
            }

            beginTest ("a taller panel does fit the extra row");
            {
                expect (layoutFits (m, { true, false },
                                    preferredHeight (m, { true, false })),
                        "a panel given the height a status row needs still reports that "
                        "it does not fit");
            }

            beginTest ("the default window height fits a status row without scrolling");
            {
                // The default used to be a literal that lost the title bar's
                // height, so every first open started already scrolling with
                // the Apply button clipped. It is derived now, and this is what
                // it has to be true of.
                //
                // FAILS IF: the default stops reserving room for the status
                // row. That row appears exactly when something has gone wrong,
                // which is when the window most needs to be readable.
                constexpr int chainRowHeight = 36;
                const auto def = defaultContentHeight (m, chainRowHeight, 3);

                expect (layoutFits (m, { true, false }, def),
                        "the default height does not fit a status row, so the window "
                        "starts scrolling the moment a plugin fails to load");

                expect (layoutFits (m, {}, def),
                        "the default height does not even fit the healthy layout");
            }

            beginTest ("the default leaves room for the chain rows it promises");
            {
                constexpr int chainRowHeight = 36;
                const auto def = defaultContentHeight (m, chainRowHeight, 3);

                expect (chainViewportHeight (m, { true, false }, def) >= 3 * chainRowHeight,
                        "a three-plugin chain does not fit the chain list at the default "
                        "size with a status row showing -- which is the case the default "
                        "was sized for");

                // And with no status row the slack goes to the chain list,
                // rather than anywhere it would look like a gap.
                expect (chainViewportHeight (m, {}, def) > chainViewportHeight (m, { true, false }, def));
            }

            beginTest ("the budget is built from the metrics, not from literals");
            {
                // Doubling every metric must move the answer, or something in
                // the sum is a magic number that will not follow a change.
                Metrics doubled;
                doubled.pad = m.pad * 2;   doubled.sectionH = m.sectionH * 2;
                doubled.rowH = m.rowH * 2; doubled.gap      = m.gap * 2;
                doubled.buttonH = m.buttonH * 2; doubled.meterH = m.meterH * 2;
                doubled.minChainH = m.minChainH * 2;

                expectEquals (fixedLayoutHeight (doubled, {}), fixedLayoutHeight (m, {}) * 2,
                              "a literal crept into the height budget, so it will not "
                              "follow a change to the metrics");
            }
        }
    };

    static PreferencesLayoutArithmeticTests preferencesLayoutArithmeticTests;

    //==========================================================================
    class ContentSizeTests final : public juce::UnitTest
    {
    public:
        ContentSizeTests()
            : juce::UnitTest ("Viewport content sizing", "PreferencesLayout") {}

        void runTest() override
        {
            beginTest ("content shorter than the viewport gets the full width");
            {
                juce::Viewport viewport;
                viewport.setSize (300, 200);

                expectEquals (contentSizeFor (viewport, 50).x, 300,
                              "the scrollbar's width was surrendered for a bar that is "
                              "not shown, so every control pinned right moves left");
            }

            beginTest ("content taller than the viewport surrenders the scrollbar width");
            {
                juce::Viewport viewport;
                viewport.setSize (300, 200);

                expectEquals (contentSizeFor (viewport, 500).x,
                              300 - viewport.getScrollBarThickness());
            }

            beginTest ("content exactly as tall as the viewport keeps the full width");
            {
                // FAILS IF: the comparison becomes >=. That surrenders eight
                // pixels in the commonest case of all.
                juce::Viewport viewport;
                viewport.setSize (300, 200);

                expectEquals (contentSizeFor (viewport, 200).x, 300);
            }

            beginTest ("the content is never shorter than the viewport");
            {
                juce::Viewport viewport;
                viewport.setSize (300, 200);

                expectEquals (contentSizeFor (viewport, 50).y, 200,
                              "the content was left shorter than the viewport, so the "
                              "space below it is unpainted");
            }

            beginTest ("a viewport with no width still gets the height right");
            {
                // FAILS IF: the zero-width early return comes back. It skipped
                // the whole calculation, which left the content at its previous
                // HEIGHT -- and the height is what decides the scroll extent,
                // while the width is cosmetic until the first layout.
                const juce::Viewport unsized;

                expectEquals (contentSizeFor (unsized, 5 * 36).y, 5 * 36,
                              "a viewport that has not been laid out left its content at "
                              "the wrong height, so it cannot be scrolled properly");

                expectEquals (contentSizeFor (unsized, 180, 200).x, 200,
                              "the minimum width floor did not hold");
            }
        }
    };

    static ContentSizeTests contentSizeTests;

    //==========================================================================
    class PanelViewportTests final : public juce::UnitTest
    {
    public:
        PanelViewportTests()
            : juce::UnitTest ("Preferences panel viewport", "PreferencesLayout") {}

        void runTest() override
        {
            beginTest ("the hook is installed by the viewport, not by its owner");
            {
                // FAILS IF: the wiring moves back out to whoever builds the
                // viewport. That is exactly what was missing, so it must not be
                // something a caller can forget.
                auto* fake = new FakePanel();
                PanelViewport<FakePanel> viewport (fake);

                expect (fake->onPreferredHeightChanged != nullptr,
                        "the panel has no way to tell its owner it grew, so an optional "
                        "row will be laid out inside the old height");
            }

            beginTest ("a panel that grows is re-heighted without another resize");
            {
                auto* fake = new FakePanel();
                PanelViewport<FakePanel> viewport (fake);
                viewport.setSize (300, 200);
                fake->setPreferredHeight (180);
                viewport.resized();

                expectEquals (fake->getHeight(), 200, "a short panel should fill the viewport");

                // The row appears: the panel now needs more than it has.
                fake->setPreferredHeight (400);
                fake->onPreferredHeightChanged();

                expectEquals (fake->getHeight(), 400,
                              "the panel grew and nothing re-heighted it, so its last "
                              "section is laid out below its own bottom edge");

                expect (viewport.getViewedComponent()->getHeight() > viewport.getMaximumVisibleHeight(),
                        "the viewport does not know it has anything to scroll to, so the "
                        "Apply button cannot be reached at all");
            }

            beginTest ("the panel is never narrower than its floor");
            {
                auto* fake = new FakePanel();
                PanelViewport<FakePanel> viewport (fake);
                viewport.setSize (50, 200);

                expect (fake->getWidth() >= 200,
                        "a very narrow window collapsed the panel instead of scrolling it");
            }
        }
    };

    static PanelViewportTests panelViewportTests;
}
