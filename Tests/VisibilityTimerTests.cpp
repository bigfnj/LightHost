#include "../Source/VisibilityTimers.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// Re-arming a timer that stopped because nobody could see it.
//
// Two categories, because the two halves of this need different things.
//
//   "VisibilityTimers"     the walk itself. No window, so it runs anywhere.
//
//   "VisibilityTimersGui"  a real DocumentWindow. The forwarding is checked
//                          unconditionally, by calling the event directly --
//                          that half is ours. JUCE's own delivery, and the
//                          claim that descendants hear nothing, are checked
//                          only when the platform really did minimise, because
//                          setMinimised is a request to a window server that a
//                          CI runner can decline. See the note in runTest.
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
        // WHY THIS FILE IS CAREFUL ABOUT WHETHER A MINIMISE HAPPENED
        //
        // setMinimised() is a request to the window server, not a state change
        // this process controls. On a macOS CI runner there is no usable window
        // session, so the request is simply dropped: nothing minimises, no
        // event is delivered, and a test that asserts on delivery fails for a
        // reason that has nothing to do with the code under test. That is what
        // happened on the first attempt.
        //
        // The subtler half is that the SECOND case below passed on macOS for
        // the wrong reason. It asserts that a descendant receives no visibility
        // event on restore, and when nothing minimises there is no restore, so
        // it passed while proving nothing at all. A test that cannot fail is
        // worse than a missing one, which this project has now learned five
        // separate times.
        //
        // So both cases ask whether the platform actually minimised, and say so
        // when it did not rather than either failing or quietly passing.

        beginTest ("the forwarding reaches a timer three levels down");
        {
            // Platform-independent, because it does not need a real minimise:
            // minimisationStateChanged is called directly, which is exactly
            // what the window server would do. This is the half that is ours.
            ForwardingWindow window;

            auto content = std::make_unique<PlainContainer>();
            auto* middle = new PlainContainer();
            auto* timed  = new CountingTimer();

            middle->addAndMakeVisible (timed);
            content->addAndMakeVisible (middle);

            window.setContentOwned (content.release(), false);
            window.setSize (300, 200);

            const auto before = timed->refreshes;

            window.minimisationStateChanged (false);

            expectEquals (timed->refreshes, before + 1,
                          "the window received the event and nothing below it "
                          "was asked to re-check its timer");
        }

        beginTest ("a real minimise and restore delivers the event JUCE promises");
        {
            // The half that is JUCE's, and the half a CI runner may decline to
            // perform. Asserted only when the window really did minimise.
            ForwardingWindow window;

            auto content = std::make_unique<PlainContainer>();
            auto* middle = new PlainContainer();
            auto* timed  = new CountingTimer();

            middle->addAndMakeVisible (timed);
            content->addAndMakeVisible (middle);

            window.setContentOwned (content.release(), false);
            window.setSize (300, 200);
            window.setVisible (true);

            const auto refreshesBefore = timed->refreshes;

            window.setMinimised (true);

            if (! window.isMinimised())
            {
                logMessage ("PLATFORM DECLINED TO MINIMISE: no usable window "
                            "session, so JUCE's delivery of "
                            "minimisationStateChanged is not exercised here. "
                            "The forwarding itself is covered by the case above, "
                            "which does not need a window server.");

                window.setVisible (false);
                expect (true);
            }
            else
            {
                window.setMinimised (false);

                expect (window.minimisationEvents > 0,
                        "the window minimised and restored, and JUCE delivered "
                        "no minimisationStateChanged -- the fix would be wired "
                        "to an event that never fires");

                expect (timed->refreshes > refreshesBefore,
                        "the window was restored and nothing below it was asked "
                        "to re-check its timer");

                window.setVisible (false);
            }
        }

        beginTest ("a restored descendant gets no visibility event of its own");
        {
            // The reason the forwarding has to exist, asserted rather than
            // assumed -- but only when a minimise actually took place. Without
            // that guard this passed on any platform that declined to minimise,
            // by comparing three unchanged counters with themselves.
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

            window.setMinimised (true);

            if (! window.isMinimised())
            {
                logMessage ("PLATFORM DECLINED TO MINIMISE: skipping the "
                            "descendant-event assertions, which would otherwise "
                            "pass by comparing unchanged counters with "
                            "themselves.");

                window.setVisible (false);
                expect (true);
            }
            else
            {
                const auto v = watcher->visibilityCalls;
                const auto h = watcher->hierarchyCalls;
                const auto r = watcher->resizeCalls;

                window.setMinimised (false);

                expectEquals (watcher->visibilityCalls, v,
                              "a descendant received visibilityChanged on "
                              "restore, which the fix assumes cannot happen");
                expectEquals (watcher->hierarchyCalls, h,
                              "a descendant received parentHierarchyChanged on restore");
                expectEquals (watcher->resizeCalls, r,
                              "a descendant was resized on restore -- the peer "
                              "is supposed to skip the bounds update while "
                              "minimised, so this cascade should not happen");

                window.setVisible (false);
            }
        }
    }
};

static VisibilityTimerGuiTests visibilityTimerGuiTests;
