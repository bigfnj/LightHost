#pragma once

#include "AudioChainList.hpp"
#include "LookAndFeel.hpp"
#include "NodeIds.hpp"
#include "PreferencesLayout.hpp"
#include "SignalMetering.hpp"
#include "UiMetrics.hpp"
#include "VisibilityTimers.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

//==============================================================================
// The signal view: the drawn level meters, the per-tap column, and the rules
// that decide what the column shows.
//
// WHY THIS IS A FILE OF ITS OWN
//
// The same reason AudioChainList.hpp is, arrived at the same way. Everything
// here is a plain juce::Component with a defaulted or one-argument constructor
// that touches no global: a test can build a SignalViewRows, hand it taps, and
// read back what it did with them. Its neighbour PreferencesContentComponent
// cannot be built by a test at all -- its constructor reaches
// JUCEApplication::getInstance(), getAppProperties() and a live
// AudioDeviceManager -- so for as long as these classes sat beside it in
// PreferencesWindow.cpp, every statement about a meter, a watch or a row was
// unreachable from the test target. That is the whole of the reason for the
// move; nothing about the drawing changed with it.
//
// meterscale came too, and had to: both meter classes read it, so leaving it
// behind would have meant either a second copy of the scale bottom and the
// colour thresholds -- the exact drift it was created to retire -- or a header
// that could not compile on its own.
//
// SectionLabel, HeaderToggle and the file-scope chainTestInFlight stayed in
// PreferencesWindow.cpp. None of them is part of the signal view, and moving a
// control here because it happens to be small is how a "signal view" header
// becomes a second junk drawer.
//==============================================================================
namespace lighthost::ui
{
    /** One row of the signal view: what the row is called, what is said about
        it on the right-hand side, and the meter it reads.

        At namespace scope rather than nested inside SignalViewRows, because
        signalViewContents below is what decides the rows and SignalViewRows
        only draws them. SignalViewRows::Tap is an alias for this, so every
        existing SignalViewPanel::Tap use reads exactly as it did.
    */
    struct SignalTap
    {
        juce::String name;
        juce::String detail;   ///< the device name on the end rows, chain latency on Output
        lighthost::metering::Meter* meter = nullptr;
    };

    /** Everything SignalViewPanel::setTaps takes, as one value.

        The flag travels with the rows because the same loop decides both.
        Returning the rows alone and leaving the caller to re-derive the flag is
        how the header and the rows would come to disagree about the same chain.
    */
    struct SignalViewContents
    {
        std::vector<SignalTap> taps;
        bool                   atProbeLimit = false;
    };

    //==============================================================================
    // The two rules that decide what the signal view shows.
    //
    // Both were inline in PreferencesContentComponent::refreshSignalView, which
    // no test can construct, so neither had ever been asserted. They are pure
    // functions over facts -- a callback, some names, some meter pointers -- in
    // the shape GraphTopology.hpp uses and for the same reason: nothing here
    // touches the graph, the device or the settings file, so every rule below
    // is checkable offline.
    //==============================================================================

    /** Which names label the signal view's rows.

        THE LABELS COME FROM THE COMMITTED CHAIN, NOT FROM THE STAGED ROWS.

        `stagedRows` is the STAGED chain: the user can reorder it or delete a
        row from it without pressing Apply, while the probes go on being indexed
        over what was committed, because reconnectGraph builds them from the
        committed list. Labelling row i from the staged list then names one
        plugin and meters another. refreshPluginChain hides that by setting the
        rows from the committed chain before it calls here; opening the view
        calls here directly, and does not. Taking both from the meters' own
        source makes them indexed identically by construction rather than by
        which caller got there first.

        A NULL CALLBACK IS THE ONLY FALLBACK CASE. AN EMPTY RESULT IS NOT ONE.

        A committed chain with nothing in it genuinely has no rows to draw, and
        falling back there puts the staged rows straight back -- which is the
        exact defect the callback exists to close, reintroduced at the one
        moment it matters most. A chain the user has just emptied and not
        applied is precisely when staged and committed disagree hardest, and the
        rows would then be labelled from a chain the graph is not running.

        The staged rows arrive as ChainRows rather than as a pre-built vector of
        names so that the signature says what the fallback IS. Given a name
        vector this function could not tell a deliberate fallback list from a
        committed one, and the caller would be free to hand it either.
    */
    [[nodiscard]] inline std::vector<juce::String> signalViewRowNames (
        const std::function<std::vector<juce::String>()>& committedChainNames,
        const ChainRows& stagedRows)
    {
        auto rowNames = committedChainNames ? committedChainNames()
                                            : std::vector<juce::String>{};

        if (! committedChainNames)
            for (const auto& staged : stagedRows)
                rowNames.push_back (staged.description.name);

        return rowNames;
    }

    /** The rows the signal view should show, and whether the names arrived
        sitting on the probe cap.

        Input is always first and Output always last, whatever the chain holds,
        so the result is never shorter than two rows. There used to be a "No
        plugins in the chain." message in the painter behind `if (taps.empty())`
        and it had never once been drawn, for exactly this reason.

        A chain row is paired to `probeMeterAt (i)` by position. WHEN THAT
        CALLBACK RETURNS NULL THE ROW IS DROPPED RATHER THAN NAMED. A named row
        with no meter draws at the floor for ever, and a row reading permanent
        silence says the plugin at that position is passing nothing -- a worse
        answer than no row at all, and the same judgement
        nodeids::cappedToProbes already makes about the same cliff.

        That skip is unreachable as the application is wired: getProbeMeter is
        total over every index getCommittedChainNames can return, because the
        latter is already capped to maxProbes. It is a guard on the contract
        between the two rather than a repair of anything visible, and the row
        names stay correct either way -- each row carries its own name, so
        dropping one does not shift the rest onto the wrong meters.

        `probeLimit` is a parameter so that a test can reach the cap without a
        thirty-two plugin chain. Nothing shipped passes anything but the
        default.
    */
    [[nodiscard]] inline SignalViewContents signalViewContents (
        const std::vector<juce::String>& committedNames,
        const juce::String& inputDeviceName,
        lighthost::metering::Meter* inputMeter,
        const juce::String& outputDetail,
        lighthost::metering::Meter* outputMeter,
        const std::function<lighthost::metering::Meter* (int)>& probeMeterAt,
        int probeLimit = lighthost::nodeids::maxProbes)
    {
        SignalViewContents contents;
        contents.taps.reserve (committedNames.size() + 2);

        // The device name, or a placeholder when none is selected. This was a
        // ternary whose two branches produced the same literal, so the row said
        // "device" whatever was connected.
        contents.taps.push_back ({ "Input",
                                   inputDeviceName.isEmpty() ? juce::String ("no device")
                                                             : inputDeviceName,
                                   inputMeter });

        for (size_t i = 0; i < committedNames.size(); ++i)
        {
            auto* meter = probeMeterAt ? probeMeterAt (static_cast<int> (i)) : nullptr;

            if (meter == nullptr)
                continue;

            contents.taps.push_back ({ "after " + committedNames[i], juce::String{}, meter });
        }

        contents.taps.push_back ({ "Output", outputDetail, outputMeter });

        // Whether the names came back sitting on the probe cap.
        //
        // getCommittedChainNames caps at nodeids::maxProbes so that the names
        // and the meters stay the same length -- so a longer chain loses its
        // tail up there, before anything here can see it, and the panel is
        // handed a list with no evidence that anything was dropped. Decided
        // here because this is the loop that pairs a name to a probe, and
        // therefore the last place that knows which rows are probes at all.
        //
        // Compared in int space rather than size_t. One side of this `>=` is
        // zero on an empty chain, and unsigned is where a stray `- 1` near a
        // `>=` wraps to four billion and reports a chain with no plugins in it
        // as sitting on the cap. The cast is explicit because warnings are
        // errors here.
        contents.atProbeLimit = static_cast<int> (committedNames.size()) >= probeLimit;

        return contents;
    }

    //==============================================================================
    // How a level is turned into a position and a colour.
    //
    // Shared, because there are two meters that draw bars -- the device meters and
    // the rows of the signal view -- and when these were open-coded in both, the
    // scale bottom and the colour thresholds each existed twice. One copy was named
    // and commented and the other was a literal inside a ternary, which is the exact
    // drift that NodeIds.hpp and state::directoryFor were created to retire.
    //==============================================================================
    namespace meterscale
    {
        inline constexpr int   refreshHz     = 25;

        /** How far a displayed bar falls between ticks.

            Derived, never written. The same ballistic exists on the audio thread as
            metering::kPeakDecayPerBlock, and when this was the hand-written literal
            1.8f the two agreed only by having been calculated on the same
            afternoon. A UI that falls faster than the held peak sags below the
            number beside it; slower, and the bar sticks above a level that has
            already gone.
        */
        inline constexpr float fallDbPerTick =
            lighthost::metering::kPeakFallDbPerSecond / static_cast<float> (refreshHz);

        inline constexpr float bottomDb      = -60.0f;  // the bottom of every bar

        /** Where a level sits along a bar. Linear in decibels over the visible
            range, which is what makes the scale marks land where their labels say.
        */
        [[nodiscard]] inline float positionOf (float decibels)
        {
            return juce::jlimit (0.0f, 1.0f, (decibels - bottomDb) / (0.0f - bottomDb));
        }

        [[nodiscard]] inline juce::Colour colourFor (float decibels)
        {
            using LAF = lighthost::ui::LookAndFeel;

            if (decibels >= -3.0f)  return juce::Colour (LAF::kHot);
            if (decibels >= -12.0f) return juce::Colour (LAF::kCaution);

            return juce::Colour (LAF::kAccent);
        }
    }

    //==============================================================================
    // SignalMeter
    //
    // A level readout for one measurement point: a bar, a peak-hold tick, the peak
    // in dBFS, and a clip badge that latches until it is clicked.
    //
    // THE ONE REPEATING TIMER IN THIS APPLICATION
    //
    // LookAndFeel.hpp promises no timers and no animation, and that promise is about
    // the idle cost of a tray-resident application. A meter cannot honour it
    // literally, so it honours the substance: the timer runs only while the
    // component is actually showing, and it stops on the way out.
    //
    // It deliberately does NOT hold a Meter::Watch. A Watch turns on the per-sample
    // sum of squares, and this component displays peak, which is measured always and
    // by SIMD. It did hold one briefly, which meant the audio thread computed an RMS
    // figure on every block for as long as this window was open and then threw it
    // away -- while two comments claimed the opposite. Only SignalViewRows, which
    // actually displays RMS, takes a Watch.
    //
    // WHY THE CLIP BADGE LATCHES
    //
    // Because the incident that produced this feature happened while nobody was
    // watching. A badge that decayed would have been clear again long before anyone
    // opened the window, which is the same as not having one.
    //==============================================================================
    class SignalMeter final : public juce::Component,
                              public juce::SettableTooltipClient,
                              private juce::Timer,
                              private lighthost::ui::VisibilityDrivenTimer
    {
    public:
        explicit SignalMeter (const juce::String& caption) : label (caption)
        {
            setTooltip ("Peak level" + (caption.isEmpty() ? juce::String()
                                                          : " (" + caption + ")")
                        + ". Click to clear a latched clip warning.");
        }

        /** The meter to read. Outlives this component: it belongs to IconMenu. */
        void setMeter (lighthost::metering::Meter* m)
        {
            meter = m;
            displayPeakDb = lighthost::metering::kFloorDb;
            clipped = false;

            updateTimerState();
            repaint();
        }

        void visibilityChanged() override      { updateTimerState(); }
        void parentHierarchyChanged() override { updateTimerState(); }

        // Neither of the two above fires when a window is restored from minimised.
        // PreferencesWindow forwards that event here.
        void refreshTimerForVisibility() override { updateTimerState(); }

        ~SignalMeter() override { stopTimer(); }

        void mouseDown (const juce::MouseEvent&) override
        {
            if (meter != nullptr)
                meter->clearClip();

            clipped = false;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto& laf = getLookAndFeel();
            const auto bg   = laf.findColour (juce::ResizableWindow::backgroundColourId);
            const auto text = laf.findColour (juce::Label::textColourId);

            auto area = getLocalBounds();

            if (label.isNotEmpty())
            {
                g.setColour (text.withAlpha (0.60f));
                g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.0f)));
                g.drawText (label, area.removeFromLeft (kLabelW),
                            juce::Justification::centredRight);
                area.removeFromLeft (6);
            }

            // Badge and number are pinned to the trailing edge so the bar keeps a
            // stable zero point as the text width changes.
            auto badge = area.removeFromRight (kBadgeW);
            auto number = area.removeFromRight (kNumberW);
            area.removeFromRight (4);

            drawTrack (g, area.withSizeKeepingCentre (area.getWidth(),
                                                      metrics::meterTrackHeight), bg);
            drawNumber (g, number, text);
            drawBadge (g, badge.reduced (0, 3), text);
        }

    private:
        void updateTimerState()
        {
            if (isShowing() && meter != nullptr)
                startTimerHz (meterscale::refreshHz);
            else
                stopTimer();
        }

        void timerCallback() override
        {
            // Re-checked here, not only in visibilityChanged. Minimising a window
            // delivers minimisationStateChanged to the top-level component ONLY --
            // descendants get no hook at all -- so a minimised Preferences window
            // would otherwise leave this timer running for something nobody can see.
            if (! isShowing())
            {
                stopTimer();
                return;
            }

            if (meter == nullptr)
                return;

            const auto r = meter->read();

            const auto previousPeak = displayPeakDb;
            const auto previousClip = clipped;

            // Rise instantly, fall slowly. A peak that vanished between two frames
            // is still the most useful number on screen for a moment.
            displayPeakDb = r.peakDb > displayPeakDb
                                ? r.peakDb
                                : juce::jmax (r.peakDb, displayPeakDb - meterscale::fallDbPerTick);

            clipped = r.clipped;

            // Repaint only when the drawn result would actually differ. The panel's
            // convention is to repaint the smallest thing that changed.
            if (clipped != previousClip || std::abs (displayPeakDb - previousPeak) > 0.09f)
                repaint();
        }

        void drawTrack (juce::Graphics& g, juce::Rectangle<int> track, juce::Colour bg) const
        {
            const auto trackF = track.toFloat();

            g.setColour (bg.darker (0.55f));
            g.fillRoundedRectangle (trackF, 2.0f);

            const auto proportion = meterscale::positionOf (displayPeakDb);

            if (proportion > 0.0f)
            {
                g.setColour (meterscale::colourFor (displayPeakDb));
                g.fillRoundedRectangle (trackF.withWidth (juce::jmax (2.0f,
                                                                      trackF.getWidth() * proportion)),
                                        2.0f);
            }

            // Scale marks at the decibel values a person actually steers by.
            g.setColour (bg.brighter (0.25f));

            for (const auto markDb : { -24.0f, -12.0f, -6.0f })
            {
                const auto x = trackF.getX() + trackF.getWidth() * meterscale::positionOf (markDb);
                g.fillRect (x, trackF.getY(), 1.0f, trackF.getHeight());
            }

            g.setColour (bg.brighter (0.10f));
            g.drawRoundedRectangle (trackF, 2.0f, 1.0f);
        }

        void drawNumber (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour text) const
        {
            const auto silent = displayPeakDb <= lighthost::metering::kFloorDb + 0.5f;

            g.setColour (silent ? text.withAlpha (0.35f) : meterscale::colourFor (displayPeakDb));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.0f)));
            g.drawText (silent ? juce::String ("--")
                               : juce::String (displayPeakDb, 1),
                        area, juce::Justification::centredRight);
        }

        void drawBadge (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour text) const
        {
            const auto areaF = area.toFloat();

            if (clipped)
            {
                g.setColour (juce::Colour (lighthost::ui::LookAndFeel::kHot));
                g.fillRoundedRectangle (areaF, 2.0f);
                g.setColour (juce::Colours::white);
            }
            else
            {
                g.setColour (text.withAlpha (0.22f));
                g.drawRoundedRectangle (areaF, 2.0f, 1.0f);
                g.setColour (text.withAlpha (0.30f));
            }

            g.setFont (juce::Font (juce::FontOptions{}.withHeight (9.5f)
                                                      .withStyle (clipped ? "Bold" : "Regular")));
            g.drawText ("CLIP", area, juce::Justification::centred);
        }

        static constexpr int kLabelW  = 26;
        static constexpr int   kNumberW       = 40;
        static constexpr int   kBadgeW        = 34;

        juce::String label;
        lighthost::metering::Meter* meter = nullptr;

        float displayPeakDb = lighthost::metering::kFloorDb;
        bool  clipped = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalMeter)
    };

    //==============================================================================
    // SignalViewRows
    //
    // The per-plugin taps, as one tall component inside the column's viewport:
    // input, then one row per plugin, then output. Each row carries a peak, an RMS
    // and the change from the row above it.
    //
    // The delta column is the reason this exists. Working out that smart:chain was
    // adding 7 dB took a paced offline render and an analysis script; the number was
    // available the whole time, one hop away in the graph.
    //
    // One timer for the whole column rather than one per row, and it only runs while
    // the column is showing -- which is also the only time the probes exist at all.
    //
    // WHY THE TIMER AND THE WATCHES ARE STILL HERE, ONE LEVEL INSIDE A VIEWPORT
    //
    // isShowing() walks to the top-level component, so it answers the same here as
    // it does on SignalViewPanel: a Viewport keeps its content holder and its viewed
    // component visible for as long as they are attached, which leaves the column's
    // own visible flag and the peer's minimised state as the only things that vary.
    // But visibilityChanged() is delivered to the component whose flag changed and
    // to NOTHING below it -- the same hole the timerCallback note below is about --
    // so hiding the column cannot reach this class on its own. SignalViewPanel
    // forwards it through setWatching, which keeps the stop synchronous instead of
    // one tick late.
    //==============================================================================
    class SignalViewRows final : public juce::Component,
                                 private juce::Timer,
                                 private lighthost::ui::VisibilityDrivenTimer
    {
    public:
        using Tap = SignalTap;

        SignalViewRows() = default;
        ~SignalViewRows() override { stopTimer(); }

        /** Replaces the rows. Called when the chain changes or the panel opens.

            endWatching(), not watches.clear(). "Watching" is two things -- the
            Meter::Watch handles that turn on the audio thread's per-sample RMS
            work, AND the 25 Hz timer that reads the result -- and clearing only
            the first left the second running over an empty watch list on a
            column nobody is looking at. It did recover: the next tick finds
            isShowing() false and calls endWatching() itself. But self-healing
            on the next tick is not the same as correct, and it is only true
            while that guard at the top of timerCallback survives; the whole
            point of stopping a timer when nothing can see it is that it is not
            running, not that it stops soon.

            The cost of the symmetry, stated rather than left to be found: on a
            column that IS showing, the timer now stops and restarts on every
            chain change, so the next frame arrives a full tick -- up to 40 ms
            -- later than it would have. A meter frame skipped at the instant
            the chain was replaced is not a diagnostic anyone can use, and the
            rows have just been rebuilt under it anyway.
        */
        void setTaps (std::vector<Tap> newTaps)
        {
            endWatching();
            taps = std::move (newTaps);
            readings.assign (taps.size(), {});

            if (isShowing())
                beginWatching();

            repaint();
        }

        /** The height every row needs. What the viewport scrolls over, and the only
            thing the column has to ask this class for when it lays out.
        */
        [[nodiscard]] int getPreferredHeight() const noexcept
        {
            return static_cast<int> (taps.size()) * kRowH;
        }

        /** The rows as they stand.

            For tests. Nothing shipped reads this -- paint() indexes `taps`
            directly -- and it is the only way to see what signalViewContents
            decided without a display to paint onto.
        */
        [[nodiscard]] const std::vector<Tap>& getTaps() const noexcept { return taps; }

        /** Whether the 25 Hz frame timer is running.

            For tests. juce::Timer is a private base here, so isTimerRunning()
            is unreachable from outside, and the half of "watching" that a
            watches.clear() used to leave behind is invisible without it.
        */
        [[nodiscard]] bool isTimerActive() const noexcept { return isTimerRunning(); }

        /** How many Meter::Watch handles are held.

            For tests. The other half of the same question: a watch is what
            turns on the audio thread's per-sample RMS work, so a leaked one is
            a cost paid for a column nobody is looking at.
        */
        [[nodiscard]] int numWatches() const noexcept
        {
            return static_cast<int> (watches.size());
        }

        /** Starts or stops the per-frame work, including the watches that turn on
            the audio thread's RMS accumulation.

            Driven from the column rather than from this class's own
            visibilityChanged, because a parent's visibility change is not delivered
            to its children. parentHierarchyChanged below still hooks the one event
            that IS delivered here -- being attached or detached.
        */
        void setWatching (bool shouldWatch)
        {
            if (shouldWatch)
                beginWatching();
            else
                endWatching();
        }

        void parentHierarchyChanged() override   { setWatching (isShowing()); }

        // Restoring from minimised delivers none of the hooks above.
        void refreshTimerForVisibility() override { setWatching (isShowing()); }

        void paint (juce::Graphics& g) override
        {
            auto& laf = getLookAndFeel();
            const auto bg   = laf.findColour (juce::ResizableWindow::backgroundColourId);
            const auto text = laf.findColour (juce::Label::textColourId);

            g.fillAll (bg.darker (0.30f));

            // There was a "No plugins in the chain." message here, behind
            // `if (taps.empty())`. It had never been drawn: the only caller,
            // refreshSignalView, always pushes an Input tap and an Output tap, so
            // taps.size() is at least 2 whatever the chain holds. An empty chain
            // correctly shows the two device rows and nothing between them.
            //
            // Deleted rather than repaired with `taps.size() <= 2`, because the
            // message would then be wrong in the other direction: the view is not
            // empty, it is showing the two things it can always show.

            // Only the rows the invalid region actually touches.
            //
            // 5.3.0 drew rows with removeFromTop until the area ran out and counted
            // the rest, because removeFromTop returns an empty rectangle once
            // exhausted and every draw then becomes a silent no-op -- so a chain
            // longer than the window simply stopped being shown, and a meter you
            // cannot see reads exactly like a meter showing silence. This component
            // is now laid out at its full height inside a viewport, so every row
            // exists and every row is reachable; what used to be a truncation is a
            // scroll position.
            //
            // Clipping the loop is what stops a 34-row chain costing 34 paintTap
            // calls on every one of the 25 frames a second the timer asks for, and
            // is the ONLY per-frame work a row scrolled out of sight sheds -- the
            // meters themselves are still read for every row, on purpose. See
            // timerCallback.
            const auto clip = g.getClipBounds();

            if (clip.isEmpty() || taps.empty())
                return;

            const int firstRow = juce::jmax (0, clip.getY() / kRowH);
            const int lastRow  = juce::jmin (static_cast<int> (taps.size()) - 1,
                                             (clip.getBottom() - 1) / kRowH);

            for (int i = firstRow; i <= lastRow; ++i)
                paintTap (g, { 0, i * kRowH, getWidth(), kRowH },
                          static_cast<size_t> (i), text, bg);
        }

    private:
        struct Row
        {
            float peakDb = lighthost::metering::kFloorDb;
            float rmsDb  = lighthost::metering::kFloorDb;
            bool  rmsValid = false;
            bool  clipped  = false;
        };

        void beginWatching()
        {
            if (! watches.empty())
                return;

            for (const auto& tap : taps)
                if (tap.meter != nullptr)
                    watches.push_back (std::make_unique<lighthost::metering::Meter::Watch> (tap.meter));

            if (! taps.empty())
                startTimerHz (meterscale::refreshHz);
        }

        void endWatching()
        {
            stopTimer();
            watches.clear();
        }

        void timerCallback() override
        {
            // See the note in SignalMeter: minimising reaches the top-level window
            // and nothing below it, so the check has to happen here. This one also
            // releases the watches, because they are what turn on the per-sample RMS
            // work -- the whole point of gating it.
            if (! isShowing())
            {
                endWatching();
                return;
            }

            bool changed = false;

            // EVERY row, not only the rows currently scrolled into view, and not
            // only the rows currently watched.
            //
            // Two things make that deliberate rather than an oversight. The fall
            // ballistics below advance per tick, so a row skipped while off screen
            // would scroll back carrying whatever peak it held when it left --
            // which for a diagnostic column is worse than the truncation this file
            // has just stopped doing. And the delta column reads readings[i - 1]:
            // gate the RMS on what is visible and the topmost visible row loses its
            // reference the moment the row above it scrolls off, so the one number
            // this panel exists for goes to "d --" while you scroll.
            //
            // The cost being avoided by gating would be the Watch, which is the
            // audio thread's per-sample sum of squares -- and that is gated, on the
            // whole column showing or not, which is the granularity that does not
            // break the delta. A Meter::read here is a relaxed load of a few atoms
            // on the message thread, 25 times a second.
            for (size_t i = 0; i < taps.size(); ++i)
            {
                if (taps[i].meter == nullptr)
                    continue;

                // Every tick regardless of whether anything is redrawn, because the
                // fall ballistics below advance per tick. NOT because the read
                // consumes anything: Meter::read is const and takes nothing away --
                // that is the property the 5.1.0 fix established, after a
                // destructive read meant two components watching one meter each saw
                // half of what arrived.
                const auto r = taps[i].meter->read();
                auto& row = readings[i];

                const auto previousPeak = row.peakDb;

                row.peakDb = r.peakDb > row.peakDb
                                 ? r.peakDb
                                 : juce::jmax (r.peakDb, row.peakDb - meterscale::fallDbPerTick);

                if (r.rmsValid)
                {
                    row.rmsDb    = r.rmsDb;
                    row.rmsValid = true;
                }

                if (row.clipped != r.clipped || std::abs (row.peakDb - previousPeak) > 0.09f)
                    changed = true;

                row.clipped = r.clipped;
            }

            if (changed)
                repaint();
        }

        void paintTap (juce::Graphics& g, juce::Rectangle<int> area, size_t index,
                       juce::Colour text, juce::Colour bg) const
        {
            const auto& tap = taps[index];
            const auto& row = readings[index];

            auto content = area.reduced (10, 3);

            // Name, and what the row is.
            auto titleRow = content.removeFromTop (14);
            g.setColour (text.withAlpha (0.92f));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.5f)));
            g.drawText (tap.name, titleRow.removeFromLeft (titleRow.getWidth() - 74),
                        juce::Justification::centredLeft);

            g.setColour (text.withAlpha (0.45f));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (10.5f)));
            g.drawText (tap.detail, titleRow, juce::Justification::centredRight);

            // Bar.
            auto bar = content.removeFromTop (7);
            g.setColour (bg.darker (0.60f));
            g.fillRoundedRectangle (bar.toFloat(), 2.0f);

            const auto proportion = meterscale::positionOf (row.peakDb);

            if (proportion > 0.0f)
            {
                g.setColour (meterscale::colourFor (row.peakDb));
                const auto barArea = bar.toFloat();

                g.fillRoundedRectangle (barArea.withWidth (
                                            juce::jmax (2.0f, barArea.getWidth() * proportion)), 2.0f);
            }

            content.removeFromTop (2);

            // Numbers, including the delta against the row above.
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (10.5f)));
            g.setColour (text.withAlpha (0.70f));
            g.drawText (numbersFor (row), content.removeFromLeft (128),
                        juce::Justification::centredLeft);

            if (index > 0)
                paintDelta (g, content, index, text);

            g.setColour (text.withAlpha (0.08f));
            g.fillRect (area.removeFromBottom (1));
        }

        [[nodiscard]] static juce::String numbersFor (const Row& row)
        {
            const auto silent = row.peakDb <= lighthost::metering::kFloorDb + 0.5f;

            auto s = juce::String ("pk ") + (silent ? juce::String ("--")
                                                    : juce::String (row.peakDb, 1));

            if (row.rmsValid)
                s += "   rms " + juce::String (row.rmsDb, 1);

            return s;
        }

        void paintDelta (juce::Graphics& g, juce::Rectangle<int> area, size_t index,
                         juce::Colour text) const
        {
            const auto& previous = readings[index - 1];
            const auto& row      = readings[index];

            // Only meaningful while both rows have an RMS and there is signal to
            // compare. A delta computed against silence is noise dressed as a
            // measurement, which is the mistake this whole feature came out of.
            if (! previous.rmsValid || ! row.rmsValid
                || previous.rmsDb <= lighthost::metering::kFloorDb + 20.0f)
            {
                g.setColour (text.withAlpha (0.25f));
                g.drawText ("d --", area, juce::Justification::centredRight);
                return;
            }

            const auto delta = row.rmsDb - previous.rmsDb;

            using LAF = lighthost::ui::LookAndFeel;
            g.setColour (delta > 1.0f  ? juce::Colour (LAF::kCaution)
                       : delta < -1.0f ? juce::Colour (0xff74d68c)
                                       : text.withAlpha (0.55f));

            g.drawText (juce::String (delta >= 0.0f ? "+" : "") + juce::String (delta, 1) + " dB",
                        area, juce::Justification::centredRight);
        }

    public:
        static constexpr int kRowH = 46;

    private:
        std::vector<Tap> taps;
        std::vector<Row> readings;
        std::vector<std::unique_ptr<lighthost::metering::Meter::Watch>> watches;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalViewRows)
    };

    //==============================================================================
    // SignalViewPanel
    //
    // The column to the right of the main panel: a header, and the rows in a
    // viewport under it.
    //
    // The rest of this file treats this class as "the column" -- kWidth is how much
    // the window grows by when it opens, PreferencesShell sizes it, and
    // setSignalViewOpen shows and hides it -- so the viewport went INSIDE it rather
    // than around it. Wrapping it from outside would have moved all of that onto a
    // new type and, worse, put the component that owns the timer one level below the
    // component whose visibility is toggled.
    //
    // The header stays outside the viewport. It labels the column and carries the
    // probe-limit note, both of which are still true at every scroll position, and a
    // heading that scrolls away is a heading you have to scroll back for.
    //
    // WHY THE PLAIN VIEWPORT AND NOT PreferencesPanelViewport
    //
    // Modelled on chainViewport. The height the rows want changes when the CHAIN
    // changes, not only when the window is resized, so there has to be an explicit
    // "the content changed, re-height it" call whichever pattern is used -- which is
    // exactly the shape chainViewport already has in updateChainListHeight, called
    // from resized() and again from the list's onChange. PreferencesPanelViewport
    // exists to OWN a heap-allocated panel handed to setContentOwned, and derives
    // its child's height from a layout that only moves on resize; the rows here are
    // a member held by value and neither of those applies, so subclassing would have
    // bought an override of resized() that still needed a second entry point.
    //
    // The one idea taken from PreferencesPanelViewport is subtracting the
    // scrollbar's width only when the bar is actually there. updateChainListHeight
    // subtracts it unconditionally, which costs the chain list nothing because its
    // width follows the window; this column is a fixed kWidth and pins its numbers
    // to the right edge, so surrendering the bar's width when no bar is shown would
    // shift every number in the common case.
    //==============================================================================
    class SignalViewPanel final : public juce::Component
    {
    public:
        using Tap = SignalViewRows::Tap;

        SignalViewPanel()
        {
            // Not owned: `rows` is a member. Viewport keeps the viewed component in
            // a WeakReference whether or not it owns it, which is what makes this
            // borrowed form safe -- and is why chainViewport further down this file
            // takes the same one.
            rowViewport.setViewedComponent (&rows, false);
            rowViewport.setScrollBarsShown (true, false);
            addAndMakeVisible (rowViewport);
        }

        /** Replaces the column. Called when the chain changes or the panel opens.

            `atProbeLimit` says the rows arrived at nodeids::maxProbes -- that chain
            positions past it carry no probe and therefore no row at all. It is
            passed in because the caller is what pairs a name to a probe; this class
            cannot tell a probe row from a device row, and was burned once already by
            a message that assumed it could.
        */
        void setTaps (std::vector<Tap> newTaps, bool atProbeLimit)
        {
            probeLimitReached = atProbeLimit;
            rows.setTaps (std::move (newTaps));
            updateRowsSize();
            repaint();
        }

        // Forwarded, not re-derived. Both of these are delivered to the component
        // whose state changed and to nothing beneath it, so the rows -- now a
        // grandchild, through the viewport's content holder -- would never hear the
        // column being hidden. isShowing() is evaluated on this component, which is
        // the one the shell actually toggles.
        void visibilityChanged() override        { rows.setWatching (isShowing()); }
        void parentHierarchyChanged() override   { rows.setWatching (isShowing()); }

        /** Whether the header is showing the probe-limit note.

            For tests. The flag is only ever drawn, so without this the one
            thing the caller tells this class that it cannot work out for
            itself can only be checked by painting.
        */
        [[nodiscard]] bool isAtProbeLimit() const noexcept { return probeLimitReached; }

        void paint (juce::Graphics& g) override
        {
            auto& laf = getLookAndFeel();
            const auto bg   = laf.findColour (juce::ResizableWindow::backgroundColourId);
            const auto text = laf.findColour (juce::Label::textColourId);

            g.fillAll (bg.darker (0.30f));

            // Header, matching the section bars in the main panel.
            auto header = getLocalBounds().removeFromTop (kHeaderH);
            g.setColour (bg.darker (0.55f));
            g.fillRect (header);
            g.setColour (text.withAlpha (0.80f));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.0f).withStyle ("Bold")));
            g.drawText ("SIGNAL VIEW", header.reduced (10, 0), juce::Justification::centredLeft);

            // What is left of 5.3.0's "N more not shown -- make the window taller".
            //
            // That counted the rows the column had and did not draw, and told the
            // user to resize. With the rows inside a viewport the count is always
            // zero and the instruction is wrong: scrolling is the answer now, and a
            // message about the window's height would be a message about a problem
            // that no longer exists.
            //
            // The accounting is not deleted, though, because there is a second
            // limit it was never about and which a scrollbar does nothing for.
            // nodeids::maxProbes caps the probes at 32, so a longer chain has
            // positions with no row -- and nothing inside this column can see that,
            // since getCommittedChainNames returns the list already capped. Hence
            // the flag from outside.
            //
            // Worded as a statement about the limit rather than a count of hidden
            // rows on purpose. A list that came back at exactly 32 is either a
            // 32-plugin chain with nothing missing or a longer one with its tail
            // dropped, and neither this class nor its caller can tell which -- so
            // naming a number of missing rows would be the same guess the old
            // message made about the window.
            if (probeLimitReached)
            {
                g.setColour (juce::Colour (lighthost::ui::LookAndFeel::kCaution));
                g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.0f)));
                g.drawText ("at the " + juce::String (lighthost::nodeids::maxProbes) + "-probe limit",
                            header.reduced (10, 0), juce::Justification::centredRight);
            }
        }

        void resized() override
        {
            rowViewport.setBounds (getLocalBounds().withTrimmedTop (kHeaderH));
            updateRowsSize();
        }

        /** Width the column is laid out at, and how much the window grows by. */
        static constexpr int kWidth   = 300;
        static constexpr int kHeaderH = 22;

    private:
        /** Sizes the rows to the width they may use and the height they need.
            Called on resize AND from setTaps, because either can change which of the
            two the viewport has to scroll over.

            Cannot oscillate, for the reason PreferencesPanelViewport gives for the
            same decision: the height the rows want comes from how many there are and
            does not depend on the width handed back below.
        */
        void updateRowsSize()
        {
            // The third of three call sites that used to compute this by hand,
            // and the one the other two were measured against. The zero-width
            // early return went with it: it skipped the HEIGHT as well as the
            // width, and the height is what decides the scroll extent.
            const auto size = lighthost::ui::prefs::contentSizeFor (
                                  rowViewport, rows.getPreferredHeight());

            rows.setSize (size.x, size.y);
        }

        // Declared before the viewport so the viewport is torn down first, while
        // the component it points at is still alive.
        SignalViewRows rows;
        juce::Viewport rowViewport;

        bool probeLimitReached = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalViewPanel)
    };
}
