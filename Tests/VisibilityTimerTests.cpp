#include "../Source/VisibilityTimers.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// Re-arming a timer that stopped because nobody could see it.
//
// Two categories, because the two halves of this need different things.
//
//   "VisibilityTimers"     the walk itself. No window, so it runs anywhere.
//
//   "VisibilityTimersGui"  a real DocumentWindow, actually minimised and
//                          actually restored, to check that JUCE delivers
//                          minimisationStateChanged at all and that nothing
//                          below the window hears about it. That second half is
//                          the load-bearing claim, and until this test existed
//                          the fix rested on reading ComponentPeer rather than
//                          on watching it happen.
//
// The bug being guarded: both Preferences meter timers stopped on minimise --
// correctly, that is what keeps a hidden window free -- and never started
// again, because a restore produces no visibilityChanged, no
// parentHierarchyChanged and no resized() on any descendant. They stayed frozen
// for the rest of the session.
//==============================================================================
namespace
{
    using lighthost::ui::VisibilityDrivenTimer;
    using lighthost::ui::refreshVisibilityTimers;

    /** Counts how many times it was asked to re-check. */
    class CountingTimer final : public juce::Component,
                                public VisibilityDrivenTimer
    {
    public:
        void refreshTimerForVisibility() override { ++refreshes; }

        int refreshes = 0;
    };

    /** A component that owns no timer, to sit between the window and the ones
        that do. The real hierarchy has three such levels.
    */
    class PlainContainer final : public juce::Component {};
}

//==============================================================================
class VisibilityTimerTests final : public juce::UnitTest
{
public:
    VisibilityTimerTests()
        : juce::UnitTest ("Visibility-driven timers", "VisibilityTimers") {}

    void runTest() override
    {
        beginTest ("the root itself is refreshed");
        {
            CountingTimer root;
            refreshVisibilityTimers (root);

            expectEquals (root.refreshes, 1);
        }

        beginTest ("a timer nested under plain containers is still reached");
        {
            // This is the case that matters: the meters sit below the window
            // inside a shell, a panel and a viewport, none of which own a timer.
            // A non-recursive walk would pass every test above and miss every
            // real one.
            PlainContainer root, middle, inner;
            CountingTimer deep;

            inner.addChildComponent (deep);
            middle.addChildComponent (inner);
            root.addChildComponent (middle);

            refreshVisibilityTimers (root);

            expectEquals (deep.refreshes, 1, "a timer three levels down was not reached");
        }

        beginTest ("every timer in the tree is refreshed, not just the first");
        {
            // The window has at least two: the device meters and the signal
            // view rows. Stopping at the first would leave one frozen, which is
            // exactly half of the original bug.
            PlainContainer root;
            CountingTimer a, b, c;

            root.addChildComponent (a);
            root.addChildComponent (b);
            root.addChildComponent (c);

            refreshVisibilityTimers (root);

            expectEquals (a.refreshes, 1);
            expectEquals (b.refreshes, 1);
            expectEquals (c.refreshes, 1);
        }

        beginTest ("a tree with no timers at all is harmless");
        {
            PlainContainer root, child;
            root.addChildComponent (child);

            refreshVisibilityTimers (root);      // must not throw or assert
            expect (true);
        }

        beginTest ("siblings of a timer are not disturbed and do not block it");
        {
            PlainContainer root, plainSibling;
            CountingTimer timed;

            root.addChildComponent (plainSibling);
            root.addChildComponent (timed);

            refreshVisibilityTimers (root);

            expectEquals (timed.refreshes, 1,
                          "a plain sibling earlier in the child list stopped the walk");
        }

        beginTest ("it is idempotent, because nothing promises the state changed");
        {
            // The window forwards the event without checking whether anything
            // actually changed, so a component may be asked twice in a row with
            // the same answer. Both implementations key off isShowing() rather
            // than off a delta, and this pins that expectation.
            CountingTimer root;

            refreshVisibilityTimers (root);
            refreshVisibilityTimers (root);
            refreshVisibilityTimers (root);

            expectEquals (root.refreshes, 3, "each call should reach the component");
        }

        beginTest ("a hidden component is still asked, and decides for itself");
        {
            // Deliberate: the walk does not filter on visibility. Each component
            // asks isShowing() itself, because that is the question its other
            // two hooks already answer, and a bool passed down from the window
            // would be a second source of truth that could disagree.
            PlainContainer root;
            CountingTimer timed;

            root.addChildComponent (timed);
            timed.setVisible (false);

            refreshVisibilityTimers (root);

            expectEquals (timed.refreshes, 1,
                          "the walk should ask regardless; filtering is the "
                          "component's job");
        }
    }
};

static VisibilityTimerTests visibilityTimerTests;

//==============================================================================
/** The half that needs a real window. See the note at the top of this file. */
class VisibilityTimerGuiTests final : public juce::UnitTest
{
public:
    VisibilityTimerGuiTests()
        : juce::UnitTest ("Visibility-driven timers (GUI)", "VisibilityTimersGui") {}

    /** A window shaped like PreferencesWindow: it forwards the one event JUCE
        gives it down to whatever is underneath.
    */
    class ForwardingWindow final : public juce::DocumentWindow
    {
    public:
        ForwardingWindow()
            : DocumentWindow ("VisibilityTimerTest", juce::Colours::black,
                              DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
        }

        void minimisationStateChanged (bool isNowMinimised) override
        {
            DocumentWindow::minimisationStateChanged (isNowMinimised);
            ++minimisationEvents;

            if (auto* content = getContentComponent())
                refreshVisibilityTimers (*content);
        }

        int minimisationEvents = 0;
    };

    void runTest() override
    {
        beginTest ("restoring a window re-arms a timer three levels down");
        {
            // The whole fix, end to end, against real JUCE rather than against
            // a reading of ComponentPeer.
            ForwardingWindow window;

            auto content = std::make_unique<PlainContainer>();
            auto* middle = new PlainContainer();
            auto* timed  = new CountingTimer();

            middle->addAndMakeVisible (timed);
            content->addAndMakeVisible (middle);

            window.setContentOwned (content.release(), false);
            window.setSize (300, 200);
            window.setVisible (true);

            const auto afterOpening = timed->refreshes;

            window.setMinimised (true);
            window.setMinimised (false);

            expect (window.minimisationEvents > 0,
                    "JUCE delivered no minimisationStateChanged at all, which "
                    "would mean the fix is wired to an event that never fires");

            expect (timed->refreshes > afterOpening,
                    "the window was restored and nothing below it was asked to "
                    "re-check its timer");

            window.setVisible (false);
        }

        beginTest ("the descendant gets no visibility event of its own on restore");
        {
            // This is the reason the forwarding has to exist, asserted rather
            // than assumed. If JUCE ever starts telling descendants directly,
            // this test goes red and the forwarding becomes redundant -- which
            // is worth being told about rather than discovering by reading.
            class VisibilityWatcher final : public juce::Component
            {
            public:
                void visibilityChanged() override      { ++visibilityCalls; }
                void parentHierarchyChanged() override { ++hierarchyCalls; }
                void resized() override                { ++resizeCalls; }

                int visibilityCalls = 0, hierarchyCalls = 0, resizeCalls = 0;
            };

            juce::DocumentWindow window ("VisibilityTimerTest2", juce::Colours::black,
                                         juce::DocumentWindow::closeButton);
            window.setUsingNativeTitleBar (true);

            auto content = std::make_unique<PlainContainer>();
            auto* watcher = new VisibilityWatcher();
            content->addAndMakeVisible (watcher);

            window.setContentOwned (content.release(), false);
            window.setSize (300, 200);
            window.setVisible (true);

            const auto v = watcher->visibilityCalls;
            const auto h = watcher->hierarchyCalls;
            const auto r = watcher->resizeCalls;

            window.setMinimised (true);
            window.setMinimised (false);

            expectEquals (watcher->visibilityCalls, v,
                          "a descendant received visibilityChanged on restore, "
                          "which the fix assumes cannot happen");
            expectEquals (watcher->hierarchyCalls, h,
                          "a descendant received parentHierarchyChanged on restore");
            expectEquals (watcher->resizeCalls, r,
                          "a descendant was resized on restore -- the peer is "
                          "supposed to skip the bounds update while minimised, "
                          "so this cascade should not happen");

            window.setVisible (false);
        }
    }
};

static VisibilityTimerGuiTests visibilityTimerGuiTests;
