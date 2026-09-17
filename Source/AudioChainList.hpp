#pragma once

#include "Lanes.hpp"
#include "LookAndFeel.hpp"
#include "PluginChainStore.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>
#include <vector>

//==============================================================================
// The staged plugin chain, as a drawn list.
//
// WHY THIS IS A FILE OF ITS OWN
//
// It was over five hundred lines inside PreferencesWindow.cpp, until then the
// largest file in the project, and the one thing in there that could be lifted
// out on its own: a plain juce::Component with a defaulted constructor that
// touches no global. Its neighbour PreferencesContentComponent reaches
// getAppProperties(), JUCEApplication::getInstance() and the AudioDeviceManager
// in its constructor, so a unit test cannot build one, and every assertion
// about row behaviour was therefore unreachable. Here, a test builds the list,
// drives it, and reads the rows back -- see Tests/AudioChainListTests.cpp.
//
// WHY ONE VECTOR OF ROWS RATHER THAN THREE PARALLEL ONES
//
// Up to 5.3.0 this held `std::vector<PluginDescription> items`,
// `std::vector<bool> bypassed` and `std::vector<int> lanes`, and a
// syncBypassedSize() whose whole job was keeping their lengths equal. Four of
// the eight mutation sites did not call it -- the delete callback, the lane
// callback, the checkbox and the drag path each re-established the invariant by
// hand, differently, and each read its two companions through a
// `i < vector.size() ? vector[i] : default` guard that turns a desync into a
// silently wrong lane or a silently un-bypassed plugin rather than a crash.
//
// One row holding all three makes the desync unrepresentable, which is why the
// guards and the sync call are gone rather than reinforced. The host's apply
// path still speaks in three vectors, so the conversion lives at the edge, in
// chainRowsFrom and chainVectorsFrom below, and nowhere else.
//==============================================================================
namespace lighthost::ui
{
    /** One plugin in the staged chain: which plugin, and the two settings a row
        can change.

        Position in the vector is the chain position, exactly as it was when
        this was three vectors. Node id and saved state are absent for the same
        reason chain::ChainEntry omits them: they are the host's bookkeeping and
        no row control touches them.
    */
    struct ChainRow
    {
        juce::PluginDescription description;
        bool                    bypassed = false;
        int                     lane     = 0;
    };

    using ChainRows = std::vector<ChainRow>;

    /** The three parallel vectors the host's apply and refresh paths still
        speak in.

        Kept as a named type so the three are copied and passed as ONE value.
        commitAllSettings depends on that: reading them separately across the
        async hop is how a lane change made between the click and the dispatch
        once landed on the wrong plugin.
    */
    struct ChainVectors
    {
        std::vector<juce::PluginDescription> chain;
        std::vector<bool>                    bypassed;
        std::vector<int>                     lanes;
    };

    /** Builds rows from the host's three vectors.

        Short bypass or lane vectors default rather than throwing, which is what
        the old syncBypassedSize did and what IconMenu::refreshPreferencesIfOpen
        relies on for a plugin that has never been given either.
    */
    [[nodiscard]] inline ChainRows chainRowsFrom (
        const std::vector<juce::PluginDescription>& chain,
        const std::vector<bool>& bypassed,
        const std::vector<int>& lanes)
    {
        ChainRows rows;
        rows.reserve (chain.size());

        for (size_t i = 0; i < chain.size(); ++i)
            rows.push_back ({ chain[i],
                              i < bypassed.size() ? bypassed[i] : false,
                              i < lanes.size()    ? lanes[i]    : 0 });

        return rows;
    }

    /** The reverse conversion, for the apply path. */
    [[nodiscard]] inline ChainVectors chainVectorsFrom (const ChainRows& rows)
    {
        ChainVectors vectors;
        vectors.chain.reserve (rows.size());
        vectors.bypassed.reserve (rows.size());
        vectors.lanes.reserve (rows.size());

        for (const auto& row : rows)
        {
            vectors.chain.push_back (row.description);
            vectors.bypassed.push_back (row.bypassed);
            vectors.lanes.push_back (row.lane);
        }

        return vectors;
    }

    /** The comparable form of a set of rows.

        Defers to chain::Store::identityOf rather than deriving a second notion
        of "the same plugin" here. A second one is exactly what the add menu used
        to carry, and it disagreed with the store's in both directions.
    */
    [[nodiscard]] inline std::vector<chain::ChainEntry> entriesFor (const ChainRows& rows)
    {
        std::vector<chain::ChainEntry> entries;
        entries.reserve (rows.size());

        for (const auto& row : rows)
            entries.push_back ({ chain::Store::identityOf (row.description),
                                 row.bypassed,
                                 row.lane });

        return entries;
    }

    //==============================================================================
    // AudioChainListComponent
    //
    // Paint-based list of the staged plugin chain.  Each row shows:
    //   [checkbox]  [plugin name]  [Lane N]  [Settings]  [drag handle ☰]
    //
    // Checkbox: active (filled blue + tick) = plugin active; unchecked = bypassed.
    // Drag handle: click-and-drag anywhere in the row (outside the controls) to
    //              reorder.  A blue drop-indicator line previews the target slot.
    //==============================================================================
    class AudioChainListComponent final : public juce::Component
    {
    public:
        static constexpr int kRowHeight = 36;

        AudioChainListComponent() = default;

        std::function<void()>     onChange;
        std::function<void (int)> onEditClicked;

        //==========================================================================
        [[nodiscard]] const ChainRows& getRows() const noexcept { return rows; }

        /** Replaces the list wholesale.

            Any half-finished interaction is abandoned, because it refers to rows
            that no longer exist: a drag in progress when the chain was replaced
            used to drop onto whatever had moved into the source index, which is
            the same class of failure the identity-resolved menus below close.
        */
        void setRows (ChainRows newRows)
        {
            rows = std::move (newRows);
            dragSourceRow = -1;
            dropLine      = -1;
            clearPressed();
            repaint();
        }

        /** Appends one plugin. The add menu greys identities already in the list,
            so this is not a place that has to de-duplicate.
        */
        void addRow (ChainRow row)
        {
            rows.push_back (std::move (row));
            repaint();
        }

        [[nodiscard]] int getPreferredHeight() const noexcept
        {
            return juce::jmax (kRowHeight, static_cast<int> (rows.size()) * kRowHeight);
        }

        //==========================================================================
        // The two asynchronous row menus resolve their target by IDENTITY, not by
        // the index that was under the pointer.
        //
        // Both menus used to capture `row` by value and re-check only that it was
        // still in bounds. A bounds check does not say the row is the same row:
        // the chain can be replaced while a menu is open -- a tray delete, a tray
        // move, a plugin re-declaring its latency and six other sites all reach
        // setChain -- so choosing "Delete" or "Lane 2" silently edited whatever
        // had moved into that position. showAddPluginMenu already captures the
        // data rather than an index; this is the same shape.


        /** Removes the row holding `identity`, or does nothing if it has gone. */
        void deleteByIdentity (const juce::String& identity)
        {
            const auto row = indexOfIdentity (identity);

            if (row < 0)
                return;

            rows.erase (rows.begin() + row);

            if (onChange) onChange();

            repaint();
        }

        /** Puts the row holding `identity` in `lane`, or does nothing if it has
            gone. The lane is clamped for the same reason nodeids::laneGain
            clamps: nothing between the menu and here validates it.
        */
        void setLaneByIdentity (const juce::String& identity, int lane)
        {
            const auto row = indexOfIdentity (identity);

            if (row < 0)
                return;

            rows[static_cast<size_t> (row)].lane = juce::jlimit (0, lighthost::kMaxLane, lane);

            if (onChange) onChange();

            repaint();
        }

        /** Moves the row at `from` so that it lands before position `insertAt`,
            measured in the list as it stands BEFORE the move.

            Split out of mouseUp so the reorder arithmetic can be tested at both
            ends of the list, which is where the `insertAt > from` adjustment
            either works or is off by one.
        */
        void moveRow (int from, int insertAt)
        {
            if (! juce::isPositiveAndBelow (from, static_cast<int> (rows.size())))
                return;

            insertAt = juce::jlimit (0, static_cast<int> (rows.size()), insertAt);

            // A drop onto either edge of the source row leaves it where it is.
            if (insertAt == from || insertAt == from + 1)
                return;

            auto moved = rows[static_cast<size_t> (from)];
            rows.erase (rows.begin() + from);

            // Erasing first shifts everything after the source down by one, so a
            // target beyond the source has to come back by one to mean the same
            // gap it did when the user let go.
            const int adjusted = (insertAt > from) ? insertAt - 1 : insertAt;
            rows.insert (rows.begin() + adjusted, std::move (moved));

            if (onChange) onChange();
        }

        //==========================================================================
        // Pointer handling, split from the juce::MouseEvent overrides.
        //
        // The overrides below unpack the event and do nothing else. Everything
        // that decides something lives in these three, because a unit test can
        // call them: building a juce::MouseEvent needs a MouseInputSource, an
        // originator component, a mouse-down position and a mouse-down time,
        // none of which say anything about the arm/fire/cancel contract being
        // checked, and all of which would have to be kept correct by hand.

        /** Begins an interaction. `menuTarget` is the screen rectangle the
            delete and lane menus are anchored to.
        */
        void pressAt (juce::Point<int> position, bool rightButton,
                      juce::Rectangle<int> menuTarget)
        {
            // Through rowAt, not by dividing here, so the two hit-test entry
            // points cannot disagree. They did: integer division truncates
            // toward zero, so a y of -10 gives row 0 and isPositiveAndBelow
            // accepts it -- a press above the first row pressed the first row,
            // while rowAt (used by the hover path) correctly reported none.
            const int row = rowAt (position);

            if (row < 0)
                return;

            if (rightButton)
            {
                showDeleteMenu (row, menuTarget);
                return;
            }

            // Lane button. The menu opens on press, which is the convention for a
            // menu, so the held state simply lasts as long as the menu is open.
            if (getLaneButtonArea (row).contains (position))
            {
                arm (Control::lane, row);
                showLaneMenu (row, menuTarget);
                return;
            }

            // Checkbox. Arms rather than toggling, for the same reason Settings
            // does below: a press dragged off the box has to cancel, and bypassing
            // a plugin by accident is audible in a way that opening a settings
            // window is not. It toggled on press and repainted the whole list.
            if (getCheckboxArea (row).contains (position))
            {
                arm (Control::checkbox, row);
                return;
            }

            // Settings button. Deliberately does not act here: it arms, and fires on
            // release, so the pressed state is visible for as long as the mouse is
            // held and releasing away from the button cancels it. That is what a
            // juce::Button does; a drawn control has to do it for itself.
            if (getEditButtonArea (row).contains (position))
            {
                arm (Control::settings, row);
                return;
            }

            // Begin drag (rest of row)
            dragSourceRow = row;
            dropLine      = row;
            repaint();
        }

        void dragTo (juce::Point<int> position)
        {
            // A held control un-presses when the pointer leaves it, so a button
            // never claims a click that is not going to be delivered.
            if (pressedControl != Control::none)
            {
                const bool inside = pressedRow >= 0
                                 && controlAt (position, pressedRow) == pressedControl;

                if (inside != pressedInside)
                {
                    pressedInside = inside;
                    repaintRow (pressedRow);
                }

                return;
            }

            if (dragSourceRow < 0)
                return;

            const int newDropLine = dropLineFor (position);

            if (newDropLine != dropLine)
            {
                dropLine = newDropLine;
                repaint();
            }
        }

        void releaseAt (juce::Point<int> position)
        {
            if (pressedControl == Control::settings)
            {
                const int  row    = pressedRow;
                const bool inside = row >= 0 && getEditButtonArea (row).contains (position);

                clearPressed();

                if (inside && row < static_cast<int> (rows.size()) && onEditClicked)
                    onEditClicked (row);

                return;
            }

            // The other half of arming the checkbox. Without a branch here the
            // press falls through to the drag path, which returns early on
            // dragSourceRow < 0 and never clears pressedControl -- so the box
            // would stay drawn held for the rest of the session.
            if (pressedControl == Control::checkbox)
            {
                const int  row    = pressedRow;
                const bool inside = row >= 0 && getCheckboxArea (row).contains (position);

                clearPressed();

                if (! inside || row >= static_cast<int> (rows.size()))
                    return;

                auto& flag = rows[static_cast<size_t> (row)].bypassed;
                flag = ! flag;

                // This repaints the whole list, because the callback installed by
                // the content component ends in chainList.repaint(). The
                // repaintRow below is therefore redundant today and is kept only
                // so that the intent survives if that callback ever stops doing a
                // full repaint. An earlier version of this comment claimed the
                // row was the only thing repainted, which was not true of either
                // version of the code.
                if (onChange) onChange();

                repaintRow (row);
                return;
            }

            if (pressedControl != Control::none)
            {
                // The lane menu is the remaining armed state, and its own
                // callback releases it. Nothing to do here, but the drag path
                // below must not run for it.
                return;
            }

            if (dragSourceRow < 0)
                return;

            const int source = dragSourceRow;

            dragSourceRow = -1;
            dropLine      = -1;

            moveRow (source, dropLineFor (position));
            repaint();
        }

        //==========================================================================
        void mouseDown (const juce::MouseEvent& e) override
        {
            pressAt (e.getPosition(), e.mods.isRightButtonDown(),
                     { e.getScreenX(), e.getScreenY(), 1, 1 });
        }

        void mouseDrag (const juce::MouseEvent& e) override { dragTo (e.getPosition()); }
        void mouseUp   (const juce::MouseEvent& e) override { releaseAt (e.getPosition()); }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int  row = rowAt (e.getPosition());
            const auto ctl = (row >= 0) ? controlAt (e.getPosition(), row) : Control::none;

            if (row == hotRow && ctl == hotControl)
                return;

            const int previous = hotRow;
            hotRow     = row;
            hotControl = ctl;

            setMouseCursor (ctl == Control::none ? juce::MouseCursor::NormalCursor
                                                 : juce::MouseCursor::PointingHandCursor);

            // Only the rows whose appearance changed, rather than the whole list.
            if (previous >= 0) repaintRow (previous);
            if (hotRow  >= 0)  repaintRow (hotRow);
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            if (hotRow < 0 && hotControl == Control::none)
                return;

            const int previous = hotRow;
            hotRow     = -1;
            hotControl = Control::none;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            if (previous >= 0) repaintRow (previous);
        }

        //==========================================================================
        void paint (juce::Graphics& g) override
        {
            auto& laf      = getLookAndFeel();
            const auto bg  = laf.findColour (juce::ResizableWindow::backgroundColourId);
            const auto txt = laf.findColour (juce::Label::textColourId);
            const int n    = static_cast<int> (rows.size());

            for (int i = 0; i < n; ++i)
            {
                const auto& entry = rows[static_cast<size_t> (i)];
                const juce::Rectangle<int> row (0, i * kRowHeight, getWidth(), kRowHeight);

                // Row background
                if (i == dragSourceRow)
                    g.setColour (bg.contrasting (0.12f).withAlpha (0.7f));
                else if (i % 2 == 0)
                    g.setColour (bg.brighter (0.06f));
                else
                    g.setColour (bg);
                g.fillRect (row);

                // ── Checkbox ───────────────────────────────────────────────────
                // Feedback in drawRowButton's vocabulary -- brightness for hot, a
                // darker fill plus an accent border for held, content nudged a
                // pixel -- and NOT HeaderToggle's, which says both "on" and "held"
                // by how much accent it washes in. That works on an empty bar, but
                // a ticked box is already a solid accent fill, so accent-for-held
                // would leave held-and-ticked indistinguishable from ticked and
                // make held-and-unticked look half-ticked. Brightness and border
                // are spare on both states, and they are already what the two
                // buttons further along the same row use.
                const auto accent    = juce::Colour (lighthost::ui::LookAndFeel::kAccent);
                const auto checkArea = getCheckboxArea (i).toFloat();
                const bool checkHot  = isHot  (i, Control::checkbox);
                const bool checkHeld = isHeld (i, Control::checkbox);

                auto checkFill = entry.bypassed ? bg.darker (0.1f) : accent;

                if (checkHeld)     checkFill = checkFill.darker (0.30f);
                else if (checkHot) checkFill = checkFill.brighter (0.22f);

                g.setColour (checkFill);
                g.fillRoundedRectangle (checkArea, 3.0f);

                // The empty box always needs an outline to read as a box at all.
                // The filled one gets one only while held, where it is the cue.
                if (entry.bypassed || checkHeld)
                {
                    g.setColour (checkHeld ? accent.withAlpha (0.95f)
                                           : txt.withAlpha (checkHot ? 0.42f : 0.25f));
                    g.drawRoundedRectangle (checkArea.reduced (0.5f), 3.0f, 1.0f);
                }

                if (! entry.bypassed)
                {
                    // White tick mark, dropping a pixel while held for the same
                    // reason drawRowButton's label does.
                    g.setColour (juce::Colours::white);
                    const float cx = checkArea.getCentreX();
                    const float cy = checkArea.getCentreY() + (checkHeld ? 1.0f : 0.0f);
                    juce::Path tick;
                    tick.startNewSubPath (cx - 4.0f, cy + 0.5f);
                    tick.lineTo (cx - 1.0f, cy + 3.5f);
                    tick.lineTo (cx + 5.0f, cy - 3.5f);
                    g.strokePath (tick, juce::PathStrokeType (1.8f));
                }

                // ── Plugin name ────────────────────────────────────────────────
                g.setColour (entry.bypassed ? txt.withAlpha (0.38f) : txt);
                g.setFont (juce::Font (juce::FontOptions{}.withHeight (13.5f)));
                g.drawFittedText (entry.description.name,
                                  getNameArea (i), juce::Justification::centredLeft, 1);

                // ── Edit button ────────────────────────────────────────────────
                drawRowButton (g, getEditButtonArea (i), "Settings", bg, txt,
                               isHot (i, Control::settings),
                               isHeld (i, Control::settings),
                               0.18f);

                // ── Lane button ────────────────────────────────────────────────
                drawRowButton (g, getLaneButtonArea (i), "Lane " + juce::String (entry.lane),
                               bg, txt,
                               isHot (i, Control::lane),
                               isHeld (i, Control::lane),
                               0.05f);

                // ── Drag handle (three horizontal bars, right edge) ────────────
                g.setColour (txt.withAlpha (0.28f));
                const int hx = getWidth() - 22;
                const int hy = row.getY() + 10;
                for (int line = 0; line < 3; ++line)
                    g.fillRect (hx, hy + line * 6, 12, 2);

                // ── Row separator ──────────────────────────────────────────────
                g.setColour (bg.darker (0.12f));
                g.drawHorizontalLine (row.getBottom() - 1, 0.0f,
                                      static_cast<float> (getWidth()));
            }

            // Blue drop-indicator line
            if (dragSourceRow >= 0 && dropLine >= 0)
            {
                g.setColour (juce::Colours::cornflowerblue);
                g.fillRect (4, dropLine * kRowHeight - 1, getWidth() - 8, 3);
            }

            // Empty-state hint
            if (rows.empty())
            {
                g.setColour (getLookAndFeel()
                                 .findColour (juce::Label::textColourId)
                                 .withAlpha (0.38f));
                g.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));
                g.drawText ("Click  \"+  Add Plugin\"  to build your chain.",
                            getLocalBounds(), juce::Justification::centred);
            }
        }

    private:

        /** Where `identity` sits now, or -1 when it has gone.

            First match. A tie cannot arise through the UI, because
            showAddPluginMenu greys an identity already in the list, so the same
            plugin cannot be staged twice -- and if one ever did arrive from
            elsewhere, editing the first is a defensible answer where editing a
            stale index is not.
        */
        [[nodiscard]] int indexOfIdentity (const juce::String& identity) const
        {
            for (size_t i = 0; i < rows.size(); ++i)
                if (chain::Store::identityOf (rows[i].description) == identity)
                    return static_cast<int> (i);

            return -1;
        }
        //==========================================================================
        // Which of the controls in a row the pointer is over, and which is held.
        //
        // These controls are painted, not juce::Buttons, because the chain is drawn
        // as a list. A drawn control gets no hover or pressed state for free, which
        // is exactly why the old "Edit" button looked inert: it was a rounded
        // rectangle and a string, identical whether or not it had been clicked.
        enum class Control { none, checkbox, lane, settings };

        void showDeleteMenu (int row, juce::Rectangle<int> menuTarget)
        {
            juce::PopupMenu m;
            m.addItem (1, "Delete");

            juce::Component::SafePointer<AudioChainListComponent> safe (this);
            const auto identity = chain::Store::identityOf (rows[static_cast<size_t> (row)].description);

            m.showMenuAsync (juce::PopupMenu::Options{}.withTargetScreenArea (menuTarget),
                             [safe, identity] (int result)
                             {
                                 if (safe == nullptr || result != 1)
                                     return;

                                 safe->deleteByIdentity (identity);
                             });
        }

        void showLaneMenu (int row, juce::Rectangle<int> menuTarget)
        {
            // Looped rather than written out, which is what Lanes.hpp said had
            // already been done. It had not: this was the one place that assigns
            // a plugin to a lane, so raising kMaxLane gave you trim sliders,
            // settings keys and graph nodes for the new lanes and no way to put
            // anything in one.
            juce::PopupMenu m;

            for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
                m.addItem (lane + 1, "Lane " + juce::String (lane));

            juce::Component::SafePointer<AudioChainListComponent> safe (this);
            const auto identity = chain::Store::identityOf (rows[static_cast<size_t> (row)].description);

            m.showMenuAsync (juce::PopupMenu::Options{}.withTargetScreenArea (menuTarget),
                             [safe, identity] (int result)
                             {
                                 if (safe == nullptr)
                                     return;

                                 // Released first, and unconditionally: the menu can be
                                 // dismissed without a selection, and the early return
                                 // below would otherwise leave the button looking held.
                                 safe->clearPressed();

                                 if (result == 0)
                                     return;

                                 safe->setLaneByIdentity (identity, result - 1);
                             });
        }

        /** Where a drop at `position` would insert. Half a row of hysteresis, so
            the indicator flips at the boundary between rows rather than at their
            tops.
        */
        [[nodiscard]] int dropLineFor (juce::Point<int> position) const noexcept
        {
            return juce::jlimit (0, static_cast<int> (rows.size()),
                                 (position.y + kRowHeight / 2) / kRowHeight);
        }

        void arm (Control control, int row)
        {
            pressedControl = control;
            pressedRow     = row;
            pressedInside  = true;
            repaintRow (row);
        }

        [[nodiscard]] bool isHot (int row, Control c) const noexcept
        {
            return hotRow == row && hotControl == c;
        }

        [[nodiscard]] bool isHeld (int row, Control c) const noexcept
        {
            return pressedRow == row && pressedControl == c && pressedInside;
        }

        void clearPressed()
        {
            const int row  = pressedRow;
            pressedRow     = -1;
            pressedControl = Control::none;
            pressedInside  = true;
            if (row >= 0) repaintRow (row);
        }

        void repaintRow (int row)
        {
            repaint (0, row * kRowHeight, getWidth(), kRowHeight);
        }

        [[nodiscard]] int rowAt (juce::Point<int> p) const noexcept
        {
            if (p.y < 0) return -1;
            const int row = p.y / kRowHeight;
            return row < static_cast<int> (rows.size()) ? row : -1;
        }

        [[nodiscard]] Control controlAt (juce::Point<int> p, int row) const noexcept
        {
            if (getCheckboxArea   (row).contains (p)) return Control::checkbox;
            if (getLaneButtonArea (row).contains (p)) return Control::lane;
            if (getEditButtonArea (row).contains (p)) return Control::settings;
            return Control::none;
        }

        /** The three states a real button has, drawn by hand. */
        void drawRowButton (juce::Graphics& g,
                            juce::Rectangle<int> area,
                            const juce::String& label,
                            juce::Colour bg,
                            juce::Colour txt,
                            bool hot,
                            bool held,
                            float baseDarken) const
        {
            auto fill = bg.darker (baseDarken);
            if (held)     fill = fill.darker (0.30f);
            else if (hot) fill = fill.brighter (0.22f);

            g.setColour (fill);
            g.fillRoundedRectangle (area.toFloat(), 3.0f);

            g.setColour (held ? juce::Colour (lighthost::ui::LookAndFeel::kAccent).withAlpha (0.95f)
                              : txt.withAlpha (hot ? 0.42f : 0.18f));
            g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 3.0f, 1.0f);

            // The label drops a pixel while held. It is a small thing, and it is most
            // of what makes a click feel like it landed.
            g.setColour (txt.withAlpha (held ? 0.95f : (hot ? 0.92f : 0.72f)));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.5f)));
            g.drawText (label, held ? area.translated (0, 1) : area,
                        juce::Justification::centred);
        }

        // Row layout, measured in from the right edge. paint() and the hit-testing
        // both read these, so the drawn control and the clickable area cannot drift
        // apart. "Settings" needs more room than "Edit" did, so everything to its
        // left moved with it.
        static constexpr int kButtonInsetY  = 7;
        static constexpr int kSettingsWidth = 66;
        static constexpr int kSettingsInset = 94;
        static constexpr int kLaneWidth     = 56;
        static constexpr int kLaneInset     = 158;
        static constexpr int kNameLeft      = 34;
        static constexpr int kNameRightGap  = 162;

        [[nodiscard]] juce::Rectangle<int> getCheckboxArea (int row) const noexcept
        {
            const int cy = row * kRowHeight + kRowHeight / 2;
            return { 8, cy - 9, 18, 18 };
        }

        [[nodiscard]] juce::Rectangle<int> getNameArea (int row) const noexcept
        {
            return { kNameLeft, row * kRowHeight,
                     juce::jmax (0, getWidth() - kNameLeft - kNameRightGap), kRowHeight };
        }

        [[nodiscard]] juce::Rectangle<int> getEditButtonArea (int row) const noexcept
        {
            return { getWidth() - kSettingsInset, row * kRowHeight + kButtonInsetY,
                     kSettingsWidth, kRowHeight - kButtonInsetY * 2 };
        }

        [[nodiscard]] juce::Rectangle<int> getLaneButtonArea (int row) const noexcept
        {
            return { getWidth() - kLaneInset, row * kRowHeight + kButtonInsetY,
                     kLaneWidth, kRowHeight - kButtonInsetY * 2 };
        }

        ChainRows rows;

        int dragSourceRow = -1;
        int dropLine      = -1;

        int     hotRow         = -1;
        Control hotControl     = Control::none;
        int     pressedRow     = -1;
        Control pressedControl = Control::none;
        bool    pressedInside  = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioChainListComponent)
    };
}
