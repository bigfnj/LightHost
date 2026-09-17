#include "../Source/AudioChainList.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// The staged chain list: its row invariant, its two asynchronous menus, its
// drag-reorder arithmetic and its arm/fire/cancel contract.
//
// WHY THESE CAN BE TESTED AT ALL
//
// The list lived inside PreferencesWindow.cpp until it was extracted, next to
// PreferencesContentComponent, which reaches getAppProperties(),
// JUCEApplication::getInstance() and a live AudioDeviceManager in its
// constructor. A unit test cannot build one of those, so nothing here had ever
// been asserted. Source/AudioChainList.hpp exists so that it can be.
//
// HEADLESS, NOT needsDisplay. The subject is a child juce::Component that is
// never put on the desktop: no peer, no window, no X11. That is the difference
// from PluginWindowGui, which builds a DocumentWindow and is therefore
// registered only on Windows and macOS.
//
// NOTE ON juce_gui_basics. LightHostTests does not name it in
// target_link_libraries and does not need to: juce_audio_processors depends on
// juce_gui_extra, which depends on juce_gui_basics, which is why
// Source/PluginWindow.cpp -- a DocumentWindow -- already compiles in this
// target. Adding the line would be harmless and would also be a claim that
// something needs it, so it is deliberately absent.
//
// WHAT THESE TESTS DO NOT TOUCH. Nothing here presses the lane button or
// right-clicks a row, because both open a real juce::PopupMenu and a console
// app has no display to put one on. The menus' *callbacks* are what carry the
// logic worth testing, and they are named methods -- deleteByIdentity and
// setLaneByIdentity -- so they are driven directly, which is also how the
// list-replaced-underneath case is reachable at all.
//==============================================================================
namespace
{
    using namespace lighthost::ui;

    juce::PluginDescription describe (const juce::String& name)
    {
        juce::PluginDescription description;
        description.name             = name;
        description.version          = "1.0.0";
        description.fileOrIdentifier = "C:/VST3/" + name + ".vst3";
        description.pluginFormatName = "VST3";
        return description;
    }

    ChainRows rowsFor (const juce::StringArray& names)
    {
        ChainRows rows;

        for (const auto& name : names)
            rows.push_back ({ describe (name), false, 0 });

        return rows;
    }

    juce::String namesOf (const ChainRows& rows)
    {
        juce::StringArray names;

        for (const auto& row : rows)
            names.add (row.description.name);

        return names.joinIntoString (",");
    }

    /** Name, bypass and lane together, so a failure says which of the three
        stopped travelling with its plugin rather than only that something did.
    */
    juce::String describeRows (const ChainRows& rows)
    {
        juce::StringArray parts;

        for (const auto& row : rows)
            parts.add (row.description.name + (row.bypassed ? "/byp" : "/on")
                       + "/L" + juce::String (row.lane));

        return parts.joinIntoString (" ");
    }

    juce::String identityOf (const juce::String& name)
    {
        return lighthost::chain::Store::identityOf (describe (name));
    }

    // The list is laid out at a fixed size so the hit rectangles are known.
    // Checkbox: x 8..26. Name area: x 34..158. Lane button: x 162..218.
    // Settings: x 226..292. Rows are 36 high.
    constexpr int kWidth = 320;

    [[nodiscard]] juce::Point<int> checkboxPoint (int row)
    {
        return { 17, row * AudioChainListComponent::kRowHeight + 18 };
    }

    [[nodiscard]] juce::Point<int> bodyPoint (int row)
    {
        return { 60, row * AudioChainListComponent::kRowHeight + 18 };
    }
}

//==============================================================================
class AudioChainListRowTests final : public juce::UnitTest
{
public:
    AudioChainListRowTests()
        : juce::UnitTest ("Chain list rows", "AudioChainList") {}

    void runTest() override
    {
        beginTest ("bypass and lane travel with their plugin through a reorder");
        {
            // The whole reason the three parallel vectors became one. Under the
            // old shape the drag path erased from `items` unconditionally and
            // from `bypassed` and `lanes` only when the index happened to be in
            // range, then inserted into all three -- so a short companion
            // vector shifted every later plugin's settings by one.
            //
            // FAILS IF: moveRow carries the description without the two
            // settings beside it. There is no second vector left to fall out of
            // step, so writing that now takes an explicit reset of the moved
            // row -- which is exactly the mutation this case was verified
            // against.
            AudioChainListComponent list;
            list.setRows ({ { describe ("A"), false, 0 },
                            { describe ("B"), true,  2 },
                            { describe ("C"), false, 1 } });

            list.moveRow (1, 3);

            expectEquals (describeRows (list.getRows()),
                          juce::String ("A/on/L0 C/on/L1 B/byp/L2"));
        }

        beginTest ("a delete does not shift the settings of the rows after it");
        {
            // FAILS IF: deleteByIdentity erases from the row vector but leaves
            // a separate settings vector alone. That was the old delete
            // callback's actual shape -- it erased from all three by hand and
            // guarded two of the erases on a bounds check.
            AudioChainListComponent list;
            list.setRows ({ { describe ("A"), false, 0 },
                            { describe ("B"), true,  2 },
                            { describe ("C"), false, 3 } });

            list.deleteByIdentity (identityOf ("A"));

            expectEquals (describeRows (list.getRows()),
                          juce::String ("B/byp/L2 C/on/L3"));
        }

        beginTest ("an added row carries its own settings and disturbs nobody");
        {
            // FAILS IF: addRow appends to one container and leaves another
            // short, which is what the add-plugin callback used to risk: it
            // pushed to `items` and `bypassed` and relied on syncBypassedSize
            // to extend `lanes`.
            AudioChainListComponent list;
            list.setRows ({ { describe ("A"), true, 3 } });

            expect (list.addRow ({ describe ("B"), false, 0 }));

            expectEquals (describeRows (list.getRows()), juce::String ("A/byp/L3 B/on/L0"));
        }

        beginTest ("adding an identity already staged is refused");
        {
            // FAILS IF: addRow stops de-duplicating and trusts the add menu's
            // greying instead.
            //
            // The greying is a snapshot taken when the menu is built, and
            // showMenuAsync runs no nested loop, so the chain can change while
            // the menu is open -- a latency report or a finished load reaches
            // setChain and merges in a row the snapshot never saw. Picking that
            // still-enabled item staged one identity twice, and every
            // identity-resolved operation then hit the FIRST copy: Delete on the
            // second removed the first, Lane set the first one's lane.
            AudioChainListComponent list;
            list.setRows ({ { describe ("A"), false, 0 },
                            { describe ("B"), true,  2 } });

            expect (! list.addRow ({ describe ("B"), false, 1 }),
                    "a duplicate identity should be refused");

            expectEquals (describeRows (list.getRows()),
                          juce::String ("A/on/L0 B/byp/L2"),
                          "the refused add must not disturb the row already there");
        }

        beginTest ("a refused add fires no change, an accepted one does");
        {
            // FAILS IF: addRow goes back to skipping onChange. It was the only
            // mutator that did, and it got away with it because the add-plugin
            // callback duplicated onChange's body by hand -- so a handler added
            // to onChange later would silently not fire on add.
            AudioChainListComponent list;
            int changes = 0;
            list.onChange = [&changes] { ++changes; };

            list.setRows ({ { describe ("A"), false, 0 } });

            expect (list.addRow ({ describe ("B"), false, 0 }));
            expectEquals (changes, 1, "an accepted add did not report the change");

            expect (! list.addRow ({ describe ("B"), false, 0 }));
            expectEquals (changes, 1, "a refused add reported a change that did not happen");
        }

        beginTest ("a checkbox toggle changes one row and only its bypass");
        {
            // FAILS IF: the toggle reaches any row but the one clicked.
            //
            // It used to reach into a second vector by index, and open with
            // `bypassed.resize (items.size(), false)` to make that safe --
            // which is the tell that its length was not trusted. Four of the
            // eight sites that changed the chain maintained it by hand, so a
            // flag really could end up belonging to a different row than the
            // one drawn beside it.
            AudioChainListComponent list;
            list.setSize (kWidth, 3 * AudioChainListComponent::kRowHeight);
            list.setRows ({ { describe ("A"), false, 0 },
                            { describe ("B"), true,  2 },
                            { describe ("C"), true,  1 } });

            list.pressAt (checkboxPoint (1), false, {});
            list.releaseAt (checkboxPoint (1));

            expectEquals (describeRows (list.getRows()),
                          juce::String ("A/on/L0 B/on/L2 C/byp/L1"));
        }
    }
};

static AudioChainListRowTests audioChainListRowTests;

//==============================================================================
class AudioChainListMenuTests final : public juce::UnitTest
{
public:
    AudioChainListMenuTests()
        : juce::UnitTest ("Chain list async menus", "AudioChainList") {}

    void runTest() override
    {
        beginTest ("delete resolves the row it was opened on, not the index");
        {
            // Both row menus are asynchronous, and the chain can be replaced
            // while one is open: nine sites reach setChain, including a plugin
            // re-declaring its latency. The old callbacks captured the row
            // index and re-checked only that it was still in bounds.
            //
            // FAILS IF: deleteByIdentity is reverted to an index. B sat at
            // index 1 when the menu opened and C sits there now, so an
            // index-based delete removes C and leaves B -- silently, because
            // the bounds check passes.
            AudioChainListComponent list;
            list.setRows (rowsFor ({ "A", "B", "C" }));

            const auto target = identityOf ("B");

            list.setRows (rowsFor ({ "B", "C", "A" }));   // replaced under the open menu
            list.deleteByIdentity (target);

            expectEquals (namesOf (list.getRows()), juce::String ("C,A"));
        }

        beginTest ("delete does nothing when its row has gone");
        {
            // FAILS IF: indexOfIdentity returns 0 rather than -1 for an
            // identity it cannot find, or the callback falls back to the
            // captured index. Either deletes a plugin the user never chose.
            AudioChainListComponent list;
            list.setRows (rowsFor ({ "A", "B", "C" }));

            const auto target = identityOf ("B");

            list.setRows (rowsFor ({ "A", "C" }));        // B deleted from the tray

            int changes = 0;
            list.onChange = [&changes] { ++changes; };

            list.deleteByIdentity (target);

            expectEquals (namesOf (list.getRows()), juce::String ("A,C"));
            expectEquals (changes, 0, "a menu that acted on nothing still reported a change");
        }

        beginTest ("the lane menu resolves the row it was opened on");
        {
            // FAILS IF: setLaneByIdentity is reverted to an index. B was at
            // index 1 and C is there now, so the lane the user picked for B
            // would land on C -- an audible routing change on a plugin they did
            // not touch.
            AudioChainListComponent list;
            list.setRows (rowsFor ({ "A", "B", "C" }));

            const auto target = identityOf ("B");

            list.setRows (rowsFor ({ "B", "C", "A" }));
            list.setLaneByIdentity (target, 2);

            expectEquals (describeRows (list.getRows()),
                          juce::String ("B/on/L2 C/on/L0 A/on/L0"));
        }

        beginTest ("the lane menu does nothing when its row has gone");
        {
            // FAILS IF: the bounds check is restored in place of the identity
            // lookup. The old code resized `lanes` to the new, shorter chain
            // and then wrote at the captured index.
            AudioChainListComponent list;
            list.setRows (rowsFor ({ "A", "B", "C" }));

            const auto target = identityOf ("B");

            list.setRows (rowsFor ({ "A", "C" }));

            int changes = 0;
            list.onChange = [&changes] { ++changes; };

            list.setLaneByIdentity (target, 3);

            expectEquals (describeRows (list.getRows()), juce::String ("A/on/L0 C/on/L0"));
            expectEquals (changes, 0);
        }

        beginTest ("a lane from outside the supported range is clamped");
        {
            // FAILS IF: the clamp goes. Nothing between a menu result and the
            // row validates the lane, and a lane beyond kMaxLane reaches
            // nodeids::laneGain, GraphTopology and the settings file, where it
            // would silently share the last lane's trim node.
            AudioChainListComponent list;
            list.setRows (rowsFor ({ "A" }));

            list.setLaneByIdentity (identityOf ("A"), lighthost::kMaxLane + 5);

            expectEquals (list.getRows()[0].lane, lighthost::kMaxLane);
        }
    }
};

static AudioChainListMenuTests audioChainListMenuTests;

//==============================================================================
class AudioChainListDragTests final : public juce::UnitTest
{
public:
    AudioChainListDragTests()
        : juce::UnitTest ("Chain list drag reorder", "AudioChainList") {}

    void runTest() override
    {
        beginTest ("the insertion arithmetic holds at both ends");
        {
            // insertAt is a GAP index in the list as it stood before the move,
            // so a target past the source has to come back by one once the
            // source is erased. That adjustment is either right or off by one,
            // and the ends are where an off-by-one shows.
            //
            // FAILS IF: the `insertAt > from` adjustment is dropped or applied
            // unconditionally. Dropping it puts a row dragged downwards one
            // place short of where it was let go; applying it both ways puts a
            // row dragged upwards one place too far.
            struct Case
            {
                const char* what;
                int from, insertAt;
                const char* expected;
            };

            const std::vector<Case> cases
            {
                { "first to last",        0, 3, "B,C,A" },
                { "last to first",        2, 0, "C,A,B" },
                { "first to second",      0, 2, "B,A,C" },
                { "last to second",       2, 1, "A,C,B" },
                { "middle to the end",    1, 3, "A,C,B" },
                { "middle to the start",  1, 0, "B,A,C" },
                { "onto its own top",     1, 1, "A,B,C" },
                { "onto its own bottom",  1, 2, "A,B,C" },
                { "past the end",         0, 9, "B,C,A" },
                { "before the start",     2, -4, "C,A,B" }
            };

            for (const auto& testCase : cases)
            {
                AudioChainListComponent list;
                list.setRows (rowsFor ({ "A", "B", "C" }));
                list.moveRow (testCase.from, testCase.insertAt);

                expectEquals (namesOf (list.getRows()), juce::String (testCase.expected),
                              juce::String ("moving the ") + testCase.what);
            }
        }

        beginTest ("a move from outside the list does nothing");
        {
            // A crash guard rather than a wrong-answer guard, and labelled as
            // one: with the isPositiveAndBelow check weakened to an upper bound
            // only, the negative case indexes the row vector at size_t(-1),
            // which is undefined rather than merely incorrect.
            //
            // It is reachable. releaseAt takes its source from a row index
            // stored at press time, and setRows can empty the list in between
            // -- nine sites reach it.
            AudioChainListComponent list;
            list.setRows (rowsFor ({ "A", "B" }));

            list.moveRow (5, 0);
            list.moveRow (-1, 1);

            expectEquals (namesOf (list.getRows()), juce::String ("A,B"));
        }

        beginTest ("a drag reports a change once, and a no-op drag reports none");
        {
            // FAILS IF: onChange moves outside the "something actually moved"
            // branch. The callback rebuilds the chain list's height and
            // repaints it, and on the Preferences panel it is the signal that
            // the staged chain differs from the committed one.
            AudioChainListComponent list;
            list.setSize (kWidth, 3 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B", "C" }));

            int changes = 0;
            list.onChange = [&changes] { ++changes; };

            list.moveRow (1, 1);
            expectEquals (changes, 0, "a drop onto the row's own position reported a change");

            list.moveRow (0, 3);
            expectEquals (changes, 1);
        }

        beginTest ("dragging a row to the end through the pointer path");
        {
            // The arithmetic above is reached from releaseAt, which turns a
            // pointer position into a gap index with half a row of hysteresis.
            //
            // FAILS IF: the half-row offset in dropLineFor is removed. The drop
            // would then need the pointer past the BOTTOM of the last row to
            // reach the final gap, so a row dragged to the end would land
            // second from last.
            AudioChainListComponent list;
            list.setSize (kWidth, 3 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B", "C" }));

            list.pressAt (bodyPoint (0), false, {});
            list.dragTo (bodyPoint (2));
            list.releaseAt (bodyPoint (2));

            expectEquals (namesOf (list.getRows()), juce::String ("B,C,A"));
        }

        beginTest ("dragging the last row to the front through the pointer path");
        {
            // FAILS IF: releaseAt stops clearing the drag source before it
            // moves anything, or clears it and then reads it. Either leaves the
            // next press starting a drag from a stale row.
            AudioChainListComponent list;
            list.setSize (kWidth, 3 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B", "C" }));

            list.pressAt (bodyPoint (2), false, {});
            list.dragTo ({ 60, 0 });
            list.releaseAt ({ 60, 0 });

            expectEquals (namesOf (list.getRows()), juce::String ("C,A,B"));

            // A release with no press must not move anything.
            list.releaseAt ({ 60, 0 });
            expectEquals (namesOf (list.getRows()), juce::String ("C,A,B"));
        }

        beginTest ("replacing the list abandons a drag in progress");
        {
            // FAILS IF: setRows leaves dragSourceRow set. The release would
            // then apply the drop to a list that no longer has the row it
            // started from, moving whatever had taken its index -- the same
            // stale-index failure the row menus were just fixed for, arriving
            // through the drag path.
            AudioChainListComponent list;
            list.setSize (kWidth, 3 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B", "C" }));

            list.pressAt (bodyPoint (0), false, {});
            list.setRows (rowsFor ({ "X", "Y", "Z" }));
            list.releaseAt (bodyPoint (2));

            expectEquals (namesOf (list.getRows()), juce::String ("X,Y,Z"));
        }
    }
};

static AudioChainListDragTests audioChainListDragTests;

//==============================================================================
class AudioChainListPressTests final : public juce::UnitTest
{
public:
    AudioChainListPressTests()
        : juce::UnitTest ("Chain list press contract", "AudioChainList") {}

    void runTest() override
    {
        beginTest ("a press above the first row does not start a drag on it");
        {
            // Integer division truncates TOWARD ZERO, so -10 / 36 is 0, not -1.
            // pressAt used to divide for itself and then ask
            // isPositiveAndBelow, which accepts 0 -- so a press in the gap
            // above the list armed a DRAG on row 0. rowAt, which the hover
            // path uses, guards y < 0 and correctly reported no row, so the
            // two hit tests disagreed about the same point.
            //
            // The drag is the observable, not the checkbox: the bad point is
            // outside getCheckboxArea(0), so it falls through to the drag
            // branch rather than toggling. Releasing further down then
            // reorders a plugin the user never grabbed.
            //
            // FAILS IF: pressAt computes its own row index again instead of
            // going through rowAt.
            AudioChainListComponent list;
            list.setSize (kWidth, 3 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B", "C" }));

            int changes = 0;
            list.onChange = [&changes] { ++changes; };

            list.pressAt (bodyPoint (0).withY (-10), false, {});
            list.releaseAt (bodyPoint (2));

            expectEquals (namesOf (list.getRows()), juce::String ("A,B,C"),
                          "a press above the list dragged the first plugin");
            expectEquals (changes, 0, "a press that missed every row still reordered");
        }

        beginTest ("the checkbox arms on press and does not toggle");
        {
            // Bypassing a plugin by accident is audible, so the box behaves
            // like a real button: it arms on press and acts on release.
            //
            // FAILS IF: the toggle moves back into pressAt, which is where
            // it started out. The assertion below would see the plugin
            // bypassed before the mouse had been released.
            AudioChainListComponent list;
            list.setSize (kWidth, 2 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B" }));

            int changes = 0;
            list.onChange = [&changes] { ++changes; };

            list.pressAt (checkboxPoint (0), false, {});

            expect (! list.getRows()[0].bypassed, "the press toggled instead of arming");
            expectEquals (changes, 0);
        }

        beginTest ("the checkbox fires on release inside the box");
        {
            // FAILS IF: the checkbox branch in releaseAt is removed. The press
            // then falls through to the drag path, which returns early on a
            // negative drag source and never clears the armed state, so the box
            // stays drawn held for the rest of the session and never toggles.
            AudioChainListComponent list;
            list.setSize (kWidth, 2 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B" }));

            int changes = 0;
            list.onChange = [&changes] { ++changes; };

            list.pressAt (checkboxPoint (0), false, {});
            list.releaseAt (checkboxPoint (0));

            expect (list.getRows()[0].bypassed);
            expectEquals (changes, 1);
        }

        beginTest ("a press dragged off the checkbox cancels");
        {
            // FAILS IF: releaseAt stops checking that the release landed inside
            // the box. Dragging away from a control is how a click is taken
            // back, and this is the one control here whose accidental use can
            // be heard.
            AudioChainListComponent list;
            list.setSize (kWidth, 2 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B" }));

            int changes = 0;
            list.onChange = [&changes] { ++changes; };

            list.pressAt (checkboxPoint (0), false, {});
            list.dragTo (bodyPoint (0));
            list.releaseAt (bodyPoint (0));

            expect (! list.getRows()[0].bypassed, "a cancelled press still toggled");
            expectEquals (changes, 0);
        }

        beginTest ("a cancelled press does not become a drag");
        {
            // FAILS IF: the armed-control branches in releaseAt stop returning,
            // so a cancelled checkbox press falls into the reorder path and
            // moves the row the user was trying NOT to touch.
            AudioChainListComponent list;
            list.setSize (kWidth, 3 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B", "C" }));

            list.pressAt (checkboxPoint (0), false, {});
            list.dragTo (bodyPoint (2));
            list.releaseAt (bodyPoint (2));

            expectEquals (namesOf (list.getRows()), juce::String ("A,B,C"));
        }

        beginTest ("the Settings button arms and fires on release, once");
        {
            // FAILS IF: onEditClicked is called from pressAt. Opening a plugin
            // editor on press means a press-and-drag-away still opens it, and
            // the editor is a third-party window that can take seconds to
            // appear.
            AudioChainListComponent list;
            list.setSize (kWidth, 2 * AudioChainListComponent::kRowHeight);
            list.setRows (rowsFor ({ "A", "B" }));

            std::vector<int> edited;
            list.onEditClicked = [&edited] (int row) { edited.push_back (row); };

            const juce::Point<int> settings { 250, AudioChainListComponent::kRowHeight + 18 };

            list.pressAt (settings, false, {});
            expect (edited.empty(), "the press opened an editor");

            list.releaseAt (settings);
            expectEquals ((int) edited.size(), 1);
            expectEquals (edited.empty() ? -1 : edited[0], 1);

            // Released away from the button: cancelled.
            list.pressAt (settings, false, {});
            list.releaseAt (bodyPoint (1));
            expectEquals ((int) edited.size(), 1, "a cancelled press still opened an editor");
        }

        beginTest ("an empty list keeps a row of height, and a press on it is inert");
        {
            // The height floor is what the empty-chain hint is drawn into. Its
            // failure is an honest wrong value:
            //
            // FAILS IF: the jmax in getPreferredHeight goes. An empty chain
            // would report zero, the viewport would size the list to nothing,
            // and "Click + Add Plugin to build your chain" -- the only thing
            // telling a first-time user what to do -- would have nowhere to be
            // drawn.
            //
            // The press below it is a different kind of check and is labelled
            // as one: its failure mode is a crash, not a wrong answer. pressAt
            // divides a pointer position by the row height and would index the
            // row vector with the result. Nothing else in the suite drives a
            // pointer into a list with no rows, which is exactly the state the
            // hint is shown in.
            AudioChainListComponent empty;
            empty.setSize (kWidth, AudioChainListComponent::kRowHeight);

            expectEquals (empty.getPreferredHeight(), AudioChainListComponent::kRowHeight);

            int changes = 0;
            empty.onChange = [&changes] { ++changes; };

            empty.pressAt (bodyPoint (0), false, {});
            empty.dragTo (bodyPoint (2));
            empty.releaseAt (bodyPoint (2));

            expect (empty.getRows().empty());
            expectEquals (changes, 0);
        }
    }
};

static AudioChainListPressTests audioChainListPressTests;
