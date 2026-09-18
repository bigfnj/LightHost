#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// Re-arming a timer that stopped because nobody could see it.
//
// WHY THIS EXISTS
//
// A component that runs a repeating timer only while it is visible is doing the
// right thing -- this application promises it costs nothing while nobody is
// looking at it. Stopping is easy: visibilityChanged() and
// parentHierarchyChanged() both arrive.
//
// Starting again after a MINIMISE does not work that way, and the reason is
// worth writing down because nothing about it is guessable:
//
//   ComponentPeer::handleMovedOrResized guards its whole bounds-propagation
//   block on `! nowMinimised`, so component.boundsRelativeToParent is never
//   updated while the window is minimised. On restore the new bounds equal the
//   old ones, so wasMoved and wasResized are both false and NO resized()
//   cascade reaches any descendant.
//
//   It then calls minimisationStateChanged() and sendVisibilityChangeMessage()
//   on the TOP-LEVEL component and on nothing below it.
//   Component::sendVisibilityChangeMessage calls visibilityChanged() on that one
//   component and notifies its listeners; descendants are told nothing.
//
// So a descendant that stopped its own timer on minimise receives no event of
// any kind when the window comes back. It stays stopped for the rest of the
// session. That is exactly what happened to the Preferences meters: minimise
// once, and both the device meters and every signal-view row froze until
// something unrelated re-triggered them.
//
// WHY A WALK RATHER THAN TWO BESPOKE PATHS
//
// Because there were two timer owners and the next one would have been a third.
// The top-level window is the only place the event arrives, so it is the only
// place that can distribute it, and a window should not have to know which of
// its great-grandchildren happen to own timers. Implementing the interface is
// the component's own business; the window just asks everything under it to
// re-check.
//
// Note what this does NOT do: it carries no "is now minimised" argument. Each
// component asks isShowing() for itself, because that is the question it
// actually needs answered and it is already the question its other two hooks
// answer. A bool passed down from the window would be a second source of truth.
//==============================================================================
namespace lighthost::ui
{
    /** Implemented by a component whose timer follows its own visibility. */
    struct VisibilityDrivenTimer
    {
        virtual ~VisibilityDrivenTimer() = default;

        /** Start or stop, according to whether this component can now be seen.

            Called when an ancestor learns something the component cannot: today
            only a restore from minimised. Must be idempotent -- it is called
            without any promise that the state actually changed.
        */
        virtual void refreshTimerForVisibility() = 0;
    };

    /** Tells every VisibilityDrivenTimer at or under `root` to re-check itself.

        Depth-first over the whole subtree, including `root`. Components that do
        not implement the interface are passed through rather than skipped, so a
        timer owner nested inside plain layout containers is still reached --
        which is the normal case here, since the meters sit several levels below
        the window inside a shell, a panel and a viewport.
    */
    inline void refreshVisibilityTimers (juce::Component& root)
    {
        if (auto* timed = dynamic_cast<VisibilityDrivenTimer*> (&root))
            timed->refreshTimerForVisibility();

        for (auto* child : root.getChildren())
            if (child != nullptr)
                refreshVisibilityTimers (*child);
    }
}
