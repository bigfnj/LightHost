#include "PreferencesWindow.h"
#include "GainProcessor.hpp"
#include "LookAndFeel.hpp"
#include "OfflineRender.hpp"
#include "SignalMetering.hpp"
#include "UiMetrics.hpp"
#include "Lanes.hpp"
#include <set>

using namespace juce;

namespace metrics = lighthost::ui::metrics;

juce::ApplicationProperties& getAppProperties();
juce::File getLogFile();

//==============================================================================
// SectionLabel
//
// Draws a subtle dark header bar labelling each section of the unified view.
//==============================================================================
class SectionLabel final : public juce::Component
{
public:
    explicit SectionLabel (const juce::String& text) : labelText (text) {}

    void paint (juce::Graphics& g) override
    {
        auto& laf      = getLookAndFeel();
        const auto bg  = laf.findColour (juce::ResizableWindow::backgroundColourId);
        const auto txt = laf.findColour (juce::Label::textColourId);

        g.setColour (bg.darker (0.22f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);

        g.setColour (txt.withAlpha (0.80f));
        g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.0f).withStyle ("Bold")));
        g.drawText (labelText, getLocalBounds().reduced (8, 0),
                    juce::Justification::centredLeft);
    }

private:
    juce::String labelText;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionLabel)
};

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
// component is actually showing, and it holds a Meter::Watch for exactly that
// long, so the expensive half of the measurement is not even computed when
// nobody is looking. With the window shut, this costs nothing at all.
//
// WHY THE CLIP BADGE LATCHES
//
// Because the incident that produced this feature happened while nobody was
// watching. A badge that decayed would have been clear again long before anyone
// opened the window, which is the same as not having one.
//==============================================================================
class SignalMeter final : public juce::Component,
                          public juce::SettableTooltipClient,
                          private juce::Timer
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
        watch.reset();
        displayPeakDb = lighthost::metering::kFloorDb;
        clipped = false;

        if (isShowing())
            beginWatching();

        repaint();
    }

    void visibilityChanged() override { updateWatchState(); }
    void parentHierarchyChanged() override { updateWatchState(); }

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
    void updateWatchState()
    {
        if (isShowing())
            beginWatching();
        else
            endWatching();
    }

    void beginWatching()
    {
        if (meter == nullptr || watch != nullptr)
            return;

        // Holding the Watch is what turns RMS measurement on. Taken here rather
        // than in the constructor so a panel that exists but is not on screen
        // costs nothing.
        watch = std::make_unique<lighthost::metering::Meter::Watch> (meter);
        startTimerHz (kRefreshHz);
    }

    void endWatching()
    {
        stopTimer();
        watch.reset();
    }

    void timerCallback() override
    {
        if (meter == nullptr)
            return;

        // read() is destructive: it takes the peak accumulated since last time.
        // So it must be called every tick even if nothing is redrawn, or the
        // next tick would report a stale maximum.
        const auto r = meter->read();

        const auto previousPeak = displayPeakDb;
        const auto previousClip = clipped;

        // Rise instantly, fall slowly. A peak that vanished between two frames
        // is still the most useful number on screen for a moment.
        displayPeakDb = r.peakDb > displayPeakDb
                            ? r.peakDb
                            : juce::jmax (r.peakDb, displayPeakDb - kFallDbPerTick);

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

        const auto proportion = positionOf (displayPeakDb);

        if (proportion > 0.0f)
        {
            g.setColour (colourFor (displayPeakDb));
            g.fillRoundedRectangle (trackF.withWidth (juce::jmax (2.0f,
                                                                  trackF.getWidth() * proportion)),
                                    2.0f);
        }

        // Scale marks at the decibel values a person actually steers by.
        g.setColour (bg.brighter (0.25f));

        for (const auto markDb : { -24.0f, -12.0f, -6.0f })
        {
            const auto x = trackF.getX() + trackF.getWidth() * positionOf (markDb);
            g.fillRect (x, trackF.getY(), 1.0f, trackF.getHeight());
        }

        g.setColour (bg.brighter (0.10f));
        g.drawRoundedRectangle (trackF, 2.0f, 1.0f);
    }

    void drawNumber (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour text) const
    {
        const auto silent = displayPeakDb <= lighthost::metering::kFloorDb + 0.5f;

        g.setColour (silent ? text.withAlpha (0.35f) : colourFor (displayPeakDb));
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

    /** Where a level sits along the bar. Linear in decibels over the visible
        range, which is what makes the scale marks land where the labels say.
    */
    [[nodiscard]] static float positionOf (float decibels)
    {
        return juce::jlimit (0.0f, 1.0f, (decibels - kRangeBottomDb)
                                             / (0.0f - kRangeBottomDb));
    }

    [[nodiscard]] static juce::Colour colourFor (float decibels)
    {
        using LAF = lighthost::ui::LookAndFeel;

        if (decibels >= -3.0f)  return juce::Colour (LAF::kHot);
        if (decibels >= -12.0f) return juce::Colour (LAF::kCaution);

        return juce::Colour (LAF::kAccent);
    }

    static constexpr int   kRefreshHz     = 25;
    static constexpr float kFallDbPerTick = 1.8f;   // ~45 dB/second
    static constexpr float kRangeBottomDb = -60.0f;
    static constexpr int   kLabelW        = 26;
    static constexpr int   kNumberW       = 40;
    static constexpr int   kBadgeW        = 34;

    juce::String label;
    lighthost::metering::Meter* meter = nullptr;
    std::unique_ptr<lighthost::metering::Meter::Watch> watch;

    float displayPeakDb = lighthost::metering::kFloorDb;
    bool  clipped = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalMeter)
};

//==============================================================================
// SignalViewPanel
//
// The per-plugin taps, as a column to the right of the main panel: input, then
// one row per plugin, then output. Each row carries a peak, an RMS and the
// change from the row above it.
//
// The delta column is the reason this exists. Working out that smart:chain was
// adding 7 dB took a paced offline render and an analysis script; the number was
// available the whole time, one hop away in the graph.
//
// One timer for the whole column rather than one per row, and it only runs while
// the column is showing -- which is also the only time the probes exist at all.
//==============================================================================
class SignalViewPanel final : public juce::Component,
                              private juce::Timer
{
public:
    struct Tap
    {
        juce::String name;
        juce::String detail;                              ///< latency, or the device
        lighthost::metering::Meter* meter = nullptr;
    };

    SignalViewPanel() = default;
    ~SignalViewPanel() override { stopTimer(); }

    /** Replaces the column. Called when the chain changes or the panel opens. */
    void setTaps (std::vector<Tap> newTaps)
    {
        watches.clear();
        taps = std::move (newTaps);
        readings.assign (taps.size(), {});

        if (isShowing())
            beginWatching();

        repaint();
    }

    void visibilityChanged() override        { updateWatchState(); }
    void parentHierarchyChanged() override   { updateWatchState(); }

    void paint (juce::Graphics& g) override
    {
        auto& laf = getLookAndFeel();
        const auto bg   = laf.findColour (juce::ResizableWindow::backgroundColourId);
        const auto text = laf.findColour (juce::Label::textColourId);

        g.fillAll (bg.darker (0.30f));

        auto area = getLocalBounds();

        // Header, matching the section bars in the main panel.
        auto header = area.removeFromTop (kHeaderH);
        g.setColour (bg.darker (0.55f));
        g.fillRect (header);
        g.setColour (text.withAlpha (0.80f));
        g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.0f).withStyle ("Bold")));
        g.drawText ("SIGNAL VIEW", header.reduced (10, 0), juce::Justification::centredLeft);

        if (taps.empty())
        {
            g.setColour (text.withAlpha (0.45f));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.5f)));
            g.drawFittedText ("No plugins in the chain.", area.reduced (12, 10),
                              juce::Justification::centredTop, 2);
            return;
        }

        for (size_t i = 0; i < taps.size(); ++i)
            paintTap (g, area.removeFromTop (kRowH), i, text, bg);
    }

private:
    struct Row
    {
        float peakDb = lighthost::metering::kFloorDb;
        float rmsDb  = lighthost::metering::kFloorDb;
        bool  rmsValid = false;
        bool  clipped  = false;
    };

    void updateWatchState()
    {
        if (isShowing())
            beginWatching();
        else
            endWatching();
    }

    void beginWatching()
    {
        if (! watches.empty())
            return;

        for (const auto& tap : taps)
            if (tap.meter != nullptr)
                watches.push_back (std::make_unique<lighthost::metering::Meter::Watch> (tap.meter));

        if (! taps.empty())
            startTimerHz (kRefreshHz);
    }

    void endWatching()
    {
        stopTimer();
        watches.clear();
    }

    void timerCallback() override
    {
        bool changed = false;

        for (size_t i = 0; i < taps.size(); ++i)
        {
            if (taps[i].meter == nullptr)
                continue;

            // Destructive read, so it happens every tick regardless of whether
            // anything is redrawn.
            const auto r = taps[i].meter->read();
            auto& row = readings[i];

            const auto previousPeak = row.peakDb;

            row.peakDb = r.peakDb > row.peakDb
                             ? r.peakDb
                             : juce::jmax (r.peakDb, row.peakDb - kFallDbPerTick);

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

        const auto proportion = juce::jlimit (0.0f, 1.0f,
                                              (row.peakDb + 60.0f) / 60.0f);

        if (proportion > 0.0f)
        {
            using LAF = lighthost::ui::LookAndFeel;
            const auto colour = row.peakDb >= -3.0f  ? juce::Colour (LAF::kHot)
                              : row.peakDb >= -12.0f ? juce::Colour (LAF::kCaution)
                                                     : juce::Colour (LAF::kAccent);
            g.setColour (colour);
            g.fillRoundedRectangle (bar.toFloat().withWidth (
                                        juce::jmax (2.0f, bar.getWidth() * proportion)), 2.0f);
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
    /** Width the column is laid out at, and how much the window grows by. */
    static constexpr int kWidth   = 300;
    static constexpr int kRowH    = 46;
    static constexpr int kHeaderH = 22;

private:
    static constexpr int   kRefreshHz     = 25;
    static constexpr float kFallDbPerTick = 1.8f;

    std::vector<Tap> taps;
    std::vector<Row> readings;
    std::vector<std::unique_ptr<lighthost::metering::Meter::Watch>> watches;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalViewPanel)
};

//==============================================================================
// HeaderToggle
//
// A drawn control for a section header bar. Painted rather than a TextButton
// because kSectH is 22 and metrics::pushButtonHeight is 28, so a real button
// does not fit -- and because the chain rows already established that a drawn
// control is what gets a hover state here. A flat rectangle with a string in it
// is what made the old Edit button look inert.
//==============================================================================
class HeaderToggle final : public juce::Component
{
public:
    explicit HeaderToggle (const juce::String& labelText) : text (labelText) {}

    std::function<void()> onClick;

    void setToggled (bool shouldBeOn)
    {
        if (on == shouldBeOn)
            return;

        on = shouldBeOn;
        repaint();
    }

    [[nodiscard]] bool isToggled() const noexcept { return on; }

    void mouseEnter (const juce::MouseEvent&) override { hot = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hot = false; repaint(); }
    void mouseDown  (const juce::MouseEvent&) override { held = true;  repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        held = false;
        repaint();

        // Fires on release inside the control, so a press dragged away is
        // cancelled -- the same contract as every other control here.
        if (onClick && getLocalBounds().contains (e.getPosition()))
            onClick();
    }

    void paint (juce::Graphics& g) override
    {
        const auto accent = juce::Colour (lighthost::ui::LookAndFeel::kAccent);
        const auto area = getLocalBounds();

        if (held || on)
        {
            g.setColour (accent.withAlpha (held ? 0.30f : 0.18f));
            g.fillRoundedRectangle (area.toFloat(), 3.0f);
        }

        g.setColour (accent.withAlpha (hot || on ? 1.0f : 0.75f));
        g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.0f)));
        g.drawText (text + (on ? "  <" : "  >"), area, juce::Justification::centred);
    }

private:
    juce::String text;
    bool on = false, hot = false, held = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderToggle)
};

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

    std::vector<juce::PluginDescription> items;
    std::vector<bool>                    bypassed;
    std::vector<int>                     lanes;
    std::function<void()>                onChange;
    std::function<void (int)>            onEditClicked;

    void syncBypassedSize()
    {
        bypassed.resize (items.size(), false);
        lanes.resize (items.size(), 0);
    }

    int getPreferredHeight() const noexcept
    {
        return juce::jmax (kRowHeight, static_cast<int> (items.size()) * kRowHeight);
    }

    void paint (juce::Graphics& g) override
    {
        auto& laf      = getLookAndFeel();
        const auto bg  = laf.findColour (juce::ResizableWindow::backgroundColourId);
        const auto txt = laf.findColour (juce::Label::textColourId);
        const int n    = static_cast<int> (items.size());

        for (int i = 0; i < n; ++i)
        {
            const juce::Rectangle<int> row (0, i * kRowHeight, getWidth(), kRowHeight);

            // Row background
            if (i == dragSourceRow)
                g.setColour (bg.contrasting (0.12f).withAlpha (0.7f));
            else if (i % 2 == 0)
                g.setColour (bg.brighter (0.06f));
            else
                g.setColour (bg);
            g.fillRect (row);

            const bool isBypassed = (static_cast<size_t> (i) < bypassed.size())
                                    && bypassed[static_cast<size_t> (i)];

            // ── Checkbox ───────────────────────────────────────────────────
            const auto checkArea = getCheckboxArea (i).toFloat();
            if (isBypassed)
            {
                g.setColour (bg.darker (0.1f));
                g.fillRoundedRectangle (checkArea, 3.0f);
                g.setColour (txt.withAlpha (0.25f));
                g.drawRoundedRectangle (checkArea.reduced (0.5f), 3.0f, 1.0f);
            }
            else
            {
                g.setColour (juce::Colour (lighthost::ui::LookAndFeel::kAccent));
                g.fillRoundedRectangle (checkArea, 3.0f);
                // White tick mark
                g.setColour (juce::Colours::white);
                const float cx = checkArea.getCentreX();
                const float cy = checkArea.getCentreY();
                juce::Path tick;
                tick.startNewSubPath (cx - 4.0f, cy + 0.5f);
                tick.lineTo (cx - 1.0f, cy + 3.5f);
                tick.lineTo (cx + 5.0f, cy - 3.5f);
                g.strokePath (tick, juce::PathStrokeType (1.8f));
            }

            // ── Plugin name ────────────────────────────────────────────────
            g.setColour (isBypassed ? txt.withAlpha (0.38f) : txt);
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (13.5f)));
            g.drawFittedText (items[static_cast<size_t> (i)].name,
                              getNameArea (i), juce::Justification::centredLeft, 1);

            // ── Edit button ────────────────────────────────────────────────
            drawRowButton (g, getEditButtonArea (i), "Settings", bg, txt,
                           isHot (i, Control::settings),
                           isHeld (i, Control::settings),
                           0.18f);
            
            // ── Lane button ────────────────────────────────────────────────
            const int ln = (static_cast<size_t> (i) < lanes.size())
                               ? lanes[static_cast<size_t> (i)] : 0;
            drawRowButton (g, getLaneButtonArea (i), "Lane " + juce::String (ln), bg, txt,
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
        if (items.empty())
        {
            g.setColour (getLookAndFeel()
                             .findColour (juce::Label::textColourId)
                             .withAlpha (0.38f));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));
            g.drawText ("Click  \"+  Add Plugin\"  to build your chain.",
                        getLocalBounds(), juce::Justification::centred);
        }
    }

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

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int row = e.y / kRowHeight;
        if (row < 0 || row >= static_cast<int> (items.size())) return;

        // Right-click → delete context menu
        if (e.mods.isRightButtonDown())
        {
            juce::PopupMenu m;
            m.addItem (1, "Delete");
            juce::Component::SafePointer<AudioChainListComponent> safe (this);
            m.showMenuAsync (
                juce::PopupMenu::Options{}.withTargetScreenArea (
                    { e.getScreenX(), e.getScreenY(), 1, 1 }),
                [safe, row] (int result)
                {
                    if (safe == nullptr) return;
                    if (result != 1) return;
                    if (row >= static_cast<int> (safe->items.size())) return;
                    safe->items.erase (safe->items.begin() + row);
                    if (row < static_cast<int> (safe->bypassed.size()))
                        safe->bypassed.erase (safe->bypassed.begin() + row);
                    if (row < static_cast<int> (safe->lanes.size()))
                        safe->lanes.erase (safe->lanes.begin() + row);
                    if (safe->onChange) safe->onChange();
                    safe->repaint();
                });
            return;
        }

        // Lane button. The menu opens on press, which is the convention for a
        // menu, so the held state simply lasts as long as the menu is open.
        if (getLaneButtonArea(row).contains(e.getPosition()))
        {
            pressedControl = Control::lane;
            pressedRow     = row;
            pressedInside  = true;
            repaintRow (row);

            // Looped rather than written out, which is what Lanes.hpp said had
            // already been done. It had not: this was the one place that assigns
            // a plugin to a lane, so raising kMaxLane gave you trim sliders,
            // settings keys and graph nodes for the new lanes and no way to put
            // anything in one.
            juce::PopupMenu m;

            for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
                m.addItem (lane + 1, "Lane " + juce::String (lane));
            juce::Component::SafePointer<AudioChainListComponent> safe (this);
            m.showMenuAsync (
                juce::PopupMenu::Options{}.withTargetScreenArea ({ e.getScreenX(), e.getScreenY(), 1, 1 }),
                [safe, row] (int result)
                {
                    if (safe == nullptr) return;

                    // Released first, and unconditionally: the menu can be
                    // dismissed without a selection, and every early return
                    // below would otherwise leave the button looking held.
                    safe->clearPressed();

                    if (result == 0) return;

                    // The menu is asynchronous, so the chain can have changed
                    // while it was open and the row captured at click time has to
                    // be re-checked. Without this, resize() below shrinks lanes to
                    // the new, shorter chain and the write lands past the end. The
                    // delete handler above already re-checks; this did not.
                    if (row < 0 || row >= static_cast<int> (safe->items.size()))
                        return;

                    safe->lanes.resize (safe->items.size(), 0);
                    safe->lanes[static_cast<size_t>(row)] = result - 1;
                    if (safe->onChange) safe->onChange();
                    safe->repaint();
                });
            return;
        }
        // Checkbox
        if (getCheckboxArea (row).contains (e.getPosition()))
        {
            bypassed.resize (items.size(), false);
            bypassed[static_cast<size_t> (row)] = !bypassed[static_cast<size_t> (row)];
            if (onChange) onChange();
            repaint();
            return;
        }

        // Settings button. Deliberately does not act here: it arms, and fires on
        // release, so the pressed state is visible for as long as the mouse is
        // held and releasing away from the button cancels it. That is what a
        // juce::Button does; a drawn control has to do it for itself.
        if (getEditButtonArea (row).contains (e.getPosition()))
        {
            pressedControl = Control::settings;
            pressedRow     = row;
            pressedInside  = true;
            repaintRow (row);
            return;
        }

        // Begin drag (rest of row)
        dragSourceRow = row;
        dropLine      = row;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // A held control un-presses when the mouse leaves it, so a button never
        // claims a click that is not going to be delivered.
        if (pressedControl != Control::none)
        {
            const bool inside = pressedRow >= 0
                             && controlAt (e.getPosition(), pressedRow) == pressedControl;

            if (inside != pressedInside)
            {
                pressedInside = inside;
                repaintRow (pressedRow);
            }
            return;
        }

        if (dragSourceRow < 0) return;
        const int newDropLine = juce::jlimit (0, static_cast<int> (items.size()),
                                              (e.y + kRowHeight / 2) / kRowHeight);
        if (newDropLine != dropLine) { dropLine = newDropLine; repaint(); }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (pressedControl == Control::settings)
        {
            const int  row    = pressedRow;
            const bool inside = row >= 0
                             && getEditButtonArea (row).contains (e.getPosition());

            clearPressed();

            if (inside && row < static_cast<int> (items.size()) && onEditClicked)
                onEditClicked (row);

            return;
        }

        if (dragSourceRow < 0) return;

        const int insertAt = juce::jlimit (0, static_cast<int> (items.size()),
                                            (e.y + kRowHeight / 2) / kRowHeight);

        if (insertAt != dragSourceRow && insertAt != dragSourceRow + 1)
        {
            const size_t src    = static_cast<size_t> (dragSourceRow);
            auto plugin         = items[src];
            const bool bypass   = (src < bypassed.size()) ? bypassed[src] : false;
            const int  lane     = (src < lanes.size()) ? lanes[src] : 0;

            items.erase   (items.begin()    + dragSourceRow);
            if (src < bypassed.size())
                bypassed.erase (bypassed.begin() + dragSourceRow);
            if (src < lanes.size())
                lanes.erase (lanes.begin() + dragSourceRow);

            const int adj = (insertAt > dragSourceRow) ? insertAt - 1 : insertAt;
            items.insert   (items.begin()    + adj, plugin);
            bypassed.insert (bypassed.begin() + adj, bypass);
            lanes.insert (lanes.begin() + adj, lane);

            if (onChange) onChange();
        }

        dragSourceRow = -1;
        dropLine      = -1;
        repaint();
    }

private:
    // Which of the controls in a row the pointer is over, and which is held.
    //
    // These controls are painted, not juce::Buttons, because the chain is drawn
    // as a list. A drawn control gets no hover or pressed state for free, which
    // is exactly why the old "Edit" button looked inert: it was a rounded
    // rectangle and a string, identical whether or not it had been clicked.
    enum class Control { none, checkbox, lane, settings };

    int dragSourceRow = -1;
    int dropLine      = -1;

    int     hotRow         = -1;
    Control hotControl     = Control::none;
    int     pressedRow     = -1;
    Control pressedControl = Control::none;
    bool    pressedInside  = true;

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
        return row < static_cast<int> (items.size()) ? row : -1;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioChainListComponent)
};

//==============================================================================
// PreferencesContentComponent
//
// Single unified signal-flow view containing four sections:
//   INPUT  →  AUDIO CHAIN  →  OUTPUT  →  DEVICE SETTINGS
//
// Device dropdowns modify AudioDeviceManager immediately (same behaviour as
// the previous AudioDeviceSelectorComponent).  Plugin chain + bypass state
// are staged until the user clicks Apply.
//==============================================================================
// True while a Chain Test render is on the stack. File scope rather than a
// member because the owning component does not outlive a Preferences close,
// and the render must not be re-entered. Message thread only.
static bool chainTestInFlight = false;

class PreferencesContentComponent final : public juce::Component,
                                          private juce::ChangeListener
{
public:
    PreferencesContentComponent (
        juce::AudioDeviceManager& dm,
        juce::KnownPluginList& knownPlugins_,
        const std::vector<juce::PluginDescription>& activeChain,
        const std::vector<bool>& bypassStates,
        const std::vector<int>& laneStates,
        PreferencesWindow::LaneTrim laneTrimIn,
        std::function<void (const std::vector<juce::PluginDescription>&,
                            const std::vector<bool>&,
                            const std::vector<int>&)> onApply,
        std::function<void (const juce::PluginDescription&)> onEditPlugin,
        std::function<int()> chainLatencySamples,
        lighthost::metering::Meter* inputMeterToUse,
        lighthost::metering::Meter* outputMeterToUse,
        std::function<void (bool)> onSignalViewToggled,
        std::function<lighthost::metering::Meter* (int)> probeMeterAt)
        : deviceManager (dm),
          knownPlugins   (knownPlugins_),
          laneTrim       (std::move (laneTrimIn)),
          onApplyFn      (std::move (onApply)),
          onEditPluginFn (std::move (onEditPlugin)),
          chainLatencyFn (std::move (chainLatencySamples))
    {
        // ── STATUS row ────────────────────────────────────────────────────────
        // Hidden until something goes wrong, then it sits above everything else.
        statusLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (12.0f)));
        statusLabel.setColour (juce::Label::textColourId, juce::Colours::orange);
        addChildComponent (statusLabel);

        showLogButton.setButtonText ("Show Log");
        showLogButton.setTooltip ("Opens the folder holding LightHost.log, which has the detail.");
        showLogButton.onClick = []
        {
            const auto log = getLogFile();

            if (log.existsAsFile())
                log.revealToUser();
            else
                log.getParentDirectory().revealToUser();
        };
        addChildComponent (showLogButton);

        // ── INPUT section ─────────────────────────────────────────────────────
        addAndMakeVisible (inputSectionLabel);
        addAndMakeVisible (inputDeviceCombo);
        addAndMakeVisible (inputChannelLabel);

        #if JUCE_WINDOWS
        // Processing audio from other applications needs a third-party virtual
        // input device, so say so instead of leaving the user staring at a silent
        // host. Hidden once one is installed.
        //
        // The reason is JUCE, not Windows, and the distinction matters because a
        // JUCE update could remove it. Windows has been able to capture another
        // application's playback since Vista: AUDCLNT_STREAMFLAGS_LOOPBACK on a
        // render endpoint gives a copy of that endpoint's mix, and Windows 10
        // 2004 added a per-process version. JUCE exposes none of it.
        // WASAPIDeviceMode is shared, exclusive and sharedLowLatency, and
        // "loopback" appears nowhere in juce_audio_devices as of the vendored
        // 9.0.1. See BACKLOG.md under Deferred for what to re-check on a bump.
        virtualInputHint.setText ("No virtual input found. Processing audio from other "
                                  "apps needs one.",
                                  juce::dontSendNotification);
        virtualInputHint.setFont (juce::Font (juce::FontOptions{}.withHeight (11.5f)));
        virtualInputHint.setColour (juce::Label::textColourId,
                                    juce::LookAndFeel::getDefaultLookAndFeel()
                                        .findColour (juce::Label::textColourId)
                                        .withAlpha (0.60f));
        addChildComponent (virtualInputHint);

        getCableButton.setButtonText ("Get VB-CABLE");
        getCableButton.setTooltip ("Opens vb-audio.com in your browser. Light Host does not "
                                   "bundle or install VB-CABLE.");
        getCableButton.onClick = []
        {
            juce::URL ("https://vb-audio.com/Cable/").launchInDefaultBrowser();
        };
        addChildComponent (getCableButton);
        #endif
        inputChannelLabel.setText ("", juce::dontSendNotification);  // set by rebuildDeviceCombos
        inputChannelLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (12.0f)));
        inputChannelLabel.setJustificationType (juce::Justification::centredRight);

        // ── AUDIO CHAIN section ───────────────────────────────────────────────
        addAndMakeVisible (chainSectionLabel);
        chainList.items    = activeChain;
        chainList.bypassed = bypassStates;
        chainList.lanes    = laneStates;
        chainList.syncBypassedSize();
        chainList.onChange = [this] { updateChainListHeight(); chainList.repaint(); };
        chainList.onEditClicked = [this] (int i)
        {
            if (onEditPluginFn && i < static_cast<int> (chainList.items.size()))
                onEditPluginFn (chainList.items[static_cast<size_t> (i)]);
        };
        chainViewport.setViewedComponent (&chainList, false);
        chainViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (chainViewport);

        addPluginButton.setButtonText ("+ Add Plugin");
        addPluginButton.onClick = [this] { showAddPluginMenu(); };
        addAndMakeVisible (addPluginButton);

        chainTestButton.setButtonText ("Chain Test");
        chainTestButton.setTooltip ("Renders an audio file through this chain offline and "
                                    "writes the result next to it, so you can hear and "
                                    "measure exactly what the chain does.");
        chainTestButton.onClick = [this] { runChainTest(); };
        addAndMakeVisible (chainTestButton);

        // ── LANE TRIM section ─────────────────────────────────────────────────
        // Lanes are fed the same input and summed at the output, so four lanes
        // carrying similar material arrive about 12 dB hot. These are the trims
        // for that. Unity by default, so an existing setup sounds unchanged.
        addAndMakeVisible (laneTrimSectionLabel);

        for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
        {
            const auto index = static_cast<size_t> (lane);

            auto& label = laneTrimLabels[index];
            label.setText ("L" + juce::String (lane), juce::dontSendNotification);
            label.setFont (juce::Font (juce::FontOptions{}.withHeight (11.5f)));
            label.setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (label);

            auto& slider = laneTrimSliders[index];
            slider.setSliderStyle (juce::Slider::LinearHorizontal);
            slider.setRange (lighthost::gain::kMinDb, lighthost::gain::kMaxDb, 0.1);
            slider.setSkewFactorFromMidPoint (-12.0);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
            slider.setTextValueSuffix (" dB");
            slider.setDoubleClickReturnValue (true, lighthost::gain::kDefaultDb);
            slider.setTooltip ("Trim for lane " + juce::String (lane)
                               + ". Double-click to return to unity.");
            slider.setValue (laneTrim.initialDb[index], juce::dontSendNotification);

            slider.onValueChange = [this, lane, index]
            {
                const auto decibels = static_cast<float> (laneTrimSliders[index].getValue());

                if (laneTrim.onChanged)
                    laneTrim.onChanged (lane, decibels);

                // Persist here only when this is not a drag. Typing a value,
                // nudging with the arrow keys and double-clicking to reset all
                // arrive this way and produce no drag-end.
                if (laneTrimSliders[index].isMouseButtonDown())
                    laneTrimDirty[index] = true;
                else
                    commitLaneTrim (lane);
            };

            slider.onDragEnd = [this, lane] { commitLaneTrim (lane); };

            addAndMakeVisible (slider);
        }

        // ── OUTPUT section ────────────────────────────────────────────────────
        addAndMakeVisible (outputSectionLabel);
        addAndMakeVisible (outputDeviceCombo);
        addAndMakeVisible (outputChannelLabel);
        outputChannelLabel.setText ("", juce::dontSendNotification);  // set by rebuildDeviceCombos
        outputChannelLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (12.0f)));
        outputChannelLabel.setJustificationType (juce::Justification::centredRight);

        // ── DEVICE SETTINGS section ───────────────────────────────────────────
        addAndMakeVisible (deviceSettingsLabel);
        addAndMakeVisible (deviceTypeHeadLabel);
        addAndMakeVisible (deviceTypeCombo);
        addAndMakeVisible (sampleRateHeadLabel);
        addAndMakeVisible (sampleRateCombo);
        addAndMakeVisible (bufferSizeHeadLabel);
        addAndMakeVisible (bufferSizeCombo);
        deviceTypeHeadLabel.setText ("Device API:", juce::dontSendNotification);
        deviceTypeHeadLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));
        deviceTypeHeadLabel.setJustificationType (juce::Justification::centredRight);
        sampleRateHeadLabel.setText ("Sample Rate:", juce::dontSendNotification);
        sampleRateHeadLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));
        sampleRateHeadLabel.setJustificationType (juce::Justification::centredRight);
        bufferSizeHeadLabel.setText ("Buffer Size:", juce::dontSendNotification);
        bufferSizeHeadLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));
        bufferSizeHeadLabel.setJustificationType (juce::Justification::centredRight);

        onSignalViewToggledFn = std::move (onSignalViewToggled);
        probeMeterAtFn         = std::move (probeMeterAt);

        // Added AFTER the section label so it is in front of it: JUCE paints and
        // hit-tests later children on top, and SectionLabel would otherwise
        // swallow the clicks.
        addAndMakeVisible (signalViewToggle);
        signalViewToggle.onClick = [this] { toggleSignalView(); };

        addAndMakeVisible (inputMeter);
        addAndMakeVisible (outputMeter);
        inputMeter.setMeter (inputMeterToUse);
        outputMeter.setMeter (outputMeterToUse);
        deviceInputMeter  = inputMeterToUse;
        deviceOutputMeter = outputMeterToUse;

        addAndMakeVisible (latencyHeadLabel);
        addAndMakeVisible (latencyValueLabel);
        latencyHeadLabel.setText ("Latency:", juce::dontSendNotification);
        latencyHeadLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));
        latencyHeadLabel.setJustificationType (juce::Justification::centredRight);
        latencyValueLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));
        latencyValueLabel.setJustificationType (juce::Justification::centredLeft);
        updateLatencyDisplay();

        // ── Apply button ──────────────────────────────────────────────────────
        applyButton.setButtonText ("Apply");
        applyButton.onClick = [this] { commitAllSettings(); };
        addAndMakeVisible (applyButton);

        // ── Version label (bottom-left) ───────────────────────────────────────
        versionLabel.setText ("v" + juce::JUCEApplication::getInstance()->getApplicationVersion(),
                              juce::dontSendNotification);
        versionLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (11.0f)));
        versionLabel.setColour (juce::Label::textColourId,
                                juce::LookAndFeel::getDefaultLookAndFeel()
                                    .findColour (juce::Label::textColourId)
                                    .withAlpha (0.45f));
        addAndMakeVisible (versionLabel);

        // ── Apply confirmation (bottom, right of the version label) ───────────
        applyFeedbackLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (11.5f)));
        applyFeedbackLabel.setJustificationType (juce::Justification::centredRight);
        applyFeedbackLabel.setColour (juce::Label::textColourId,
                                     juce::Colour (lighthost::ui::LookAndFeel::kAccent));
        addAndMakeVisible (applyFeedbackLabel);

        // Device combos are staged — no immediate apply on change.
        // ChangeListener below keeps them in sync with external device events.

        deviceManager.addChangeListener (this);
        rebuildDeviceCombos();
    }

    ~PreferencesContentComponent() override
    {
        deviceManager.removeChangeListener (this);

        // A trim changed by a route that produces no drag-end is written here, so
        // closing the window cannot lose it.
        for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
            if (laneTrimDirty[static_cast<size_t> (lane)])
                commitLaneTrim (lane);
    }

    void setChain (const std::vector<juce::PluginDescription>& chain,
                   const std::vector<bool>& bypass, const std::vector<int>& laneStates)
    {
        chainList.items    = chain;
        chainList.bypassed = bypass; chainList.lanes = laneStates;
        chainList.syncBypassedSize();
        updateChainListHeight();
        chainList.repaint();

        // The chain just changed, so its declared latency probably did too.
        updateLatencyDisplay();
    }

    /** Rebuilds the signal view's rows from the current chain.

        Called when the view opens and whenever the chain changes, because the
        probes are recreated on both and the meters they belong to move with
        them.
    */
    void refreshSignalView (SignalViewPanel& panel, int chainLatencySamplesNow)
    {
        std::vector<SignalViewPanel::Tap> taps;
        taps.reserve (chainList.items.size() + 2);

        const auto rate = [this]
        {
            auto* device = deviceManager.getCurrentAudioDevice();
            return device != nullptr ? device->getCurrentSampleRate() : 0.0;
        }();

        const auto asMs = [rate] (int samples)
        {
            return rate > 0.0 ? juce::String (samples * 1000.0 / rate, 1) + " ms"
                              : juce::String{};
        };

        taps.push_back ({ "Input", inputDeviceCombo.getText().isEmpty()
                                       ? juce::String ("device")
                                       : juce::String ("device"),
                          deviceInputMeter });

        for (size_t i = 0; i < chainList.items.size(); ++i)
            taps.push_back ({ "after " + chainList.items[i].name,
                              juce::String{},
                              probeMeterAtFn ? probeMeterAtFn (static_cast<int> (i)) : nullptr });

        taps.push_back ({ "Output", asMs (chainLatencySamplesNow), deviceOutputMeter });

        panel.setTaps (std::move (taps));
    }

    [[nodiscard]] bool isSignalViewOpen() const { return signalViewToggle.isToggled(); }

    /** Writes a lane trim to disk, once, if it has moved since the last write. */
    void commitLaneTrim (int lane)
    {
        const auto index = static_cast<size_t> (lane);

        if (laneTrim.onCommitted)
            laneTrim.onCommitted (lane, static_cast<float> (laneTrimSliders[index].getValue()));

        laneTrimDirty[index] = false;
    }

    /** Shows a confirmation beside Apply, then clears it. */
    void setApplyFeedback (const juce::String& message)
    {
        applyFeedbackLabel.setText (message, juce::dontSendNotification);

        // A one-shot, not a repeating timer: nothing here animates, and the idle
        // cost of this window has to stay at zero. The generation counter means a
        // second Apply during the window replaces the message rather than having
        // the first one clear the second one early.
        ++applyFeedbackGeneration;
        const auto generation = applyFeedbackGeneration;
        juce::Component::SafePointer<PreferencesContentComponent> safe (this);

        juce::Timer::callAfterDelay (kApplyFeedbackMs, [safe, generation]
        {
            if (safe == nullptr) return;
            if (safe->applyFeedbackGeneration != generation) return;

            safe->applyFeedbackLabel.setText ({}, juce::dontSendNotification);
        });
    }

    /** Shows or hides the status row, and relays out around it. */
    void setStatusMessage (const juce::String& message)
    {
        const bool show = message.isNotEmpty();

        statusLabel.setText (show ? "Problem: " + message : juce::String(),
                             juce::dontSendNotification);
        statusLabel.setVisible (show);
        showLogButton.setVisible (show);

        resized();
    }

    /** Height of everything except the chain viewport, which is the only section
        that stretches. Depends on which optional rows are showing.
    */
    [[nodiscard]] int fixedLayoutHeight() const
    {
        #if JUCE_WINDOWS
        const int hintH = virtualInputHint.isVisible() ? kRowH + kGap : 0;
        #else
        const int hintH = 0;
        #endif

        const int statusH = statusLabel.isVisible() ? kRowH + kGap : 0;

        const int aboveChain = kPad
            + statusH                           // status row, when something failed
            + kSectH + kGap + kRowH + kGap     // INPUT
        + metrics::meterHeight + kGap      // input meter
            + hintH                             // virtual-input hint, when shown
            + kSectH + kGap;                    // AUDIO CHAIN label

        const int belowChain = kGap + kRowH + kGap           // Add Plugin row
            + kSectH + kGap + kRowH + kGap                    // LANE TRIM
            + kSectH + kGap + kRowH + kGap                    // OUTPUT
        + metrics::meterHeight + kGap                     // output meter
            + kSectH + kGap + kRowH + kGap + kRowH + kGap + kRowH + kGap
            + kRowH + kGap                                    // DEVICE SETTINGS
            + kBtnH + kPad;

        return aboveChain + belowChain;
    }

    /** The shortest this panel can be laid out at without a section losing its
        height. The scrolling viewport that holds it never sizes it below this, so
        a window too short for the layout scrolls instead of quietly eating the
        Apply button.
    */
    [[nodiscard]] int getPreferredHeight() const
    {
        return fixedLayoutHeight() + kMinChainH;
    }

    void resized() override
    {
        const int chainViewH = juce::jmax (kMinChainH, getHeight() - fixedLayoutHeight());

        auto area = getLocalBounds().reduced (kPad, kPad);

        // ── STATUS ─────────────────────────────────────────────────────────────
        if (statusLabel.isVisible())
        {
            auto row = area.removeFromTop (kRowH);
            showLogButton.setBounds (
                metrics::pushButton (row.removeFromRight (kShowLogW + 8), kShowLogW));
            statusLabel.setBounds (row.reduced (2, 0));
            area.removeFromTop (kGap);
        }

        // ── INPUT ──────────────────────────────────────────────────────────────
        inputSectionLabel.setBounds (area.removeFromTop (kSectH));
        area.removeFromTop (kGap);
        {
            auto row = area.removeFromTop (kRowH);
            inputChannelLabel.setBounds (row.removeFromRight (130));
            inputDeviceCombo.setBounds  (row.reduced (0, 2));
        }
        area.removeFromTop (kGap);
        inputMeter.setBounds (area.removeFromTop (metrics::meterHeight));
        area.removeFromTop (kGap);

        #if JUCE_WINDOWS
        if (virtualInputHint.isVisible())
        {
            auto row = area.removeFromTop (kRowH);
            getCableButton.setBounds (
                metrics::pushButton (row.removeFromRight (kGetCableW + 8), kGetCableW));
            virtualInputHint.setBounds (row.reduced (2, 0));
            area.removeFromTop (kGap);
        }
        #endif

        // ── AUDIO CHAIN ────────────────────────────────────────────────────────
        {
            auto header = area.removeFromTop (kSectH);
            signalViewToggle.setBounds (header.removeFromRight (96).reduced (3, 2));
            chainSectionLabel.setBounds (header);
        }
        area.removeFromTop (kGap);
        chainViewport.setBounds (area.removeFromTop (chainViewH));
        updateChainListHeight();
        area.removeFromTop (kGap);
        {
            // One row, a button pinned to each end: the destructive-ish test on the
            // left, the thing you reach for constantly on the right.
            auto row = area.removeFromTop (kRowH);
            chainTestButton.setBounds (
                metrics::pushButton (row.removeFromLeft (kChainTestW + 8), kChainTestW));
            addPluginButton.setBounds (
                metrics::pushButton (row.removeFromRight (kAddPluginW + 8), kAddPluginW));
        }
        area.removeFromTop (kGap);

        // ── LANE TRIM ─────────────────────────────────────────────────────────
        laneTrimSectionLabel.setBounds (area.removeFromTop (kSectH));
        area.removeFromTop (kGap);
        {
            auto row = area.removeFromTop (kRowH);
            const int cellWidth = row.getWidth() / lighthost::kNumLanes;

            for (int lane = 0; lane < lighthost::kNumLanes; ++lane)
            {
                const auto index = static_cast<size_t> (lane);

                // The last cell takes the remainder, so rounding does not leave a
                // gap on the right at awkward widths.
                auto cell = (lane == lighthost::kMaxLane) ? row
                                                          : row.removeFromLeft (cellWidth);

                laneTrimLabels[index].setBounds (cell.removeFromLeft (22));
                laneTrimSliders[index].setBounds (cell.reduced (2, 3));
            }
        }
        area.removeFromTop (kGap);

        // ── OUTPUT ────────────────────────────────────────────────────────────
        outputSectionLabel.setBounds (area.removeFromTop (kSectH));
        area.removeFromTop (kGap);
        {
            auto row = area.removeFromTop (kRowH);
            outputChannelLabel.setBounds (row.removeFromRight (130));
            outputDeviceCombo.setBounds  (row.reduced (0, 2));
        }
        area.removeFromTop (kGap);
        outputMeter.setBounds (area.removeFromTop (metrics::meterHeight));
        area.removeFromTop (kGap);

        // ── DEVICE SETTINGS ───────────────────────────────────────────────────
        deviceSettingsLabel.setBounds (area.removeFromTop (kSectH));
        area.removeFromTop (kGap);
        {
            constexpr int kLabelW = 90;
            auto row = area.removeFromTop (kRowH);
            deviceTypeHeadLabel.setBounds (row.removeFromLeft (kLabelW));
            deviceTypeCombo.setBounds     (row.removeFromLeft (280).reduced (4, 2));
        }
        area.removeFromTop (kGap);
        {
            constexpr int kLabelW = 90;
            auto row = area.removeFromTop (kRowH);
            sampleRateHeadLabel.setBounds (row.removeFromLeft (kLabelW));
            sampleRateCombo.setBounds     (row.removeFromLeft (220).reduced (4, 2));
        }
        area.removeFromTop (kGap);
        {
            constexpr int kLabelW = 90;
            auto row = area.removeFromTop (kRowH);
            bufferSizeHeadLabel.setBounds (row.removeFromLeft (kLabelW));
            bufferSizeCombo.setBounds     (row.removeFromLeft (280).reduced (4, 2));
        }
        area.removeFromTop (kGap);
        {
            // Directly under Buffer Size on purpose. Buffer Size is the only
            // delay figure this panel used to show, and it is not the one that
            // matters: a 10 ms buffer sat above a 94 ms plugin chain with
            // nothing on screen to say so.
            constexpr int kLabelW = 90;
            auto row = area.removeFromTop (kRowH);
            latencyHeadLabel.setBounds  (row.removeFromLeft (kLabelW));
            latencyValueLabel.setBounds (row.removeFromLeft (340).reduced (4, 2));
        }
        area.removeFromTop (kGap);

        // ── Buttons ───────────────────────────────────────────────────────────
        {
            auto row = area.removeFromTop (kBtnH);
            applyButton.setBounds (
                metrics::pushButton (row.removeFromRight (kApplyW + 8), kApplyW));
            versionLabel.setBounds  (row.removeFromLeft (120).reduced (4, 6));
            applyFeedbackLabel.setBounds (row.reduced (6, 6));
        }
    }

private:
    juce::AudioDeviceManager& deviceManager;
    juce::KnownPluginList&    knownPlugins;

    PreferencesWindow::LaneTrim laneTrim;

    std::function<void (const std::vector<juce::PluginDescription>&,
                        const std::vector<bool>&,
                        const std::vector<int>&)>    onApplyFn;
    std::function<void (const juce::PluginDescription&)> onEditPluginFn;
    std::function<int()> chainLatencyFn;

    // INPUT
    SectionLabel   inputSectionLabel   { "  INPUT" };
    juce::ComboBox inputDeviceCombo;
    juce::Label    inputChannelLabel;

    #if JUCE_WINDOWS
    juce::Label      virtualInputHint;
    juce::TextButton getCableButton;
    #endif

    // AUDIO CHAIN
    SectionLabel            chainSectionLabel  { "  AUDIO CHAIN" };
    AudioChainListComponent chainList;
    juce::Viewport          chainViewport;
    juce::TextButton        addPluginButton;
    juce::TextButton        chainTestButton;

    // Kept alive across the async file choosers.
    std::unique_ptr<juce::FileChooser> chainTestChooser;

    // STATUS
    juce::Label      statusLabel;
    juce::TextButton showLogButton;

    // Layout metrics, shared by resized() and the height it reports to its
    // scrolling viewport, so the two cannot disagree.
    static constexpr int kPad       = 10;
    static constexpr int kSectH     = 22;
    static constexpr int kRowH      = 28;
    static constexpr int kGap       = 6;
    static constexpr int kBtnH      = 40;
    static constexpr int kMinChainH = 80;

    // Push-button widths. The height is shared application-wide and lives in
    // UiMetrics.hpp; only the widths are local, and they differ solely because
    // the labels do.
    static constexpr int kApplyW     = 82;
    static constexpr int kAddPluginW = 116;
    static constexpr int kChainTestW = 100;
    static constexpr int kShowLogW   = 90;
    static constexpr int kGetCableW  = 110;

    // Three of these buttons share a kRowH row with a label, and
    // fixedLayoutHeight accounts for those rows as kRowH. A push button taller
    // than the row would overflow it and silently disagree with the reported
    // height, which is the one thing the metrics comment above promises cannot
    // happen.
    static_assert (metrics::pushButtonHeight <= kRowH,
                   "a push button must fit the row it is laid out in");

    // LANE TRIM
    SectionLabel  laneTrimSectionLabel { "  LANE TRIM" };
    std::array<juce::Label,  lighthost::kNumLanes> laneTrimLabels;
    std::array<juce::Slider, lighthost::kNumLanes> laneTrimSliders;
    std::array<bool,         lighthost::kNumLanes> laneTrimDirty {};

    // OUTPUT
    SectionLabel   outputSectionLabel   { "  OUTPUT" };
    juce::ComboBox outputDeviceCombo;
    juce::Label    outputChannelLabel;

    // DEVICE SETTINGS
    SectionLabel   deviceSettingsLabel  { "  DEVICE SETTINGS" };
    juce::Label    deviceTypeHeadLabel;
    juce::ComboBox deviceTypeCombo;
    juce::Label    sampleRateHeadLabel;
    juce::ComboBox sampleRateCombo;
    juce::Label    bufferSizeHeadLabel;
    juce::ComboBox bufferSizeCombo;
    juce::Label    latencyHeadLabel;
    juce::Label    latencyValueLabel;

    // One per device, each sitting with the device it describes.
    SignalMeter inputMeter  { "in" };
    SignalMeter outputMeter { "out" };

    HeaderToggle signalViewToggle { "Signal view" };
    lighthost::metering::Meter* deviceInputMeter  = nullptr;
    lighthost::metering::Meter* deviceOutputMeter = nullptr;
    std::function<void (bool)> onSignalViewToggledFn;
    std::function<lighthost::metering::Meter* (int)> probeMeterAtFn;

    // Apply button, version label, and the transient Apply confirmation
    juce::TextButton applyButton;
    juce::Label      versionLabel;
    juce::Label      applyFeedbackLabel;

    static constexpr int kApplyFeedbackMs = 2600;
    int applyFeedbackGeneration = 0;


    //--------------------------------------------------------------------------

    void rebuildDeviceCombos()
    {
        const auto setup = deviceManager.getAudioDeviceSetup();

        // Device API (type)
        deviceTypeCombo.clear (juce::dontSendNotification);
        const auto currentTypeName = deviceManager.getCurrentAudioDeviceType();
        int typeSel = 1;
        const auto& types = deviceManager.getAvailableDeviceTypes();
        for (int i = 0; i < types.size(); ++i)
        {
            deviceTypeCombo.addItem (types[i]->getTypeName(), i + 1);
            if (types[i]->getTypeName() == currentTypeName) typeSel = i + 1;
        }
        deviceTypeCombo.setSelectedId (typeSel, juce::dontSendNotification);

        // Input devices
        inputDeviceCombo.clear (juce::dontSendNotification);
        juce::StringArray inputNames;
        for (auto* type : deviceManager.getAvailableDeviceTypes())
            for (const auto& name : type->getDeviceNames (true))
                inputNames.addIfNotAlreadyThere (name);
        int inputSel = 1;
        for (int i = 0; i < inputNames.size(); ++i)
        {
            inputDeviceCombo.addItem (inputNames[i], i + 1);
            if (inputNames[i] == setup.inputDeviceName) inputSel = i + 1;
        }
        if (inputNames.isEmpty())
            inputDeviceCombo.addItem ("(no input devices)", 1);
        inputDeviceCombo.setSelectedId (inputSel, juce::dontSendNotification);

        // Output devices
        outputDeviceCombo.clear (juce::dontSendNotification);
        juce::StringArray outputNames;
        for (auto* type : deviceManager.getAvailableDeviceTypes())
            for (const auto& name : type->getDeviceNames (false))
                outputNames.addIfNotAlreadyThere (name);
        int outputSel = 1;
        for (int i = 0; i < outputNames.size(); ++i)
        {
            outputDeviceCombo.addItem (outputNames[i], i + 1);
            if (outputNames[i] == setup.outputDeviceName) outputSel = i + 1;
        }
        if (outputNames.isEmpty())
            outputDeviceCombo.addItem ("(no output devices)", 1);
        outputDeviceCombo.setSelectedId (outputSel, juce::dontSendNotification);

        rebuildSampleRateCombo();
        rebuildBufferSizeCombo();
        updateChannelLabels();
        updateVirtualInputHint (inputNames);
    }

    /** Shows the "get a virtual input" hint only when no virtual input device is
        installed. Once the user has one, this never appears again.

        Matching is by device-name substring because there is no API that reports
        "this endpoint is virtual". Kept to the drivers people actually use; a
        false negative just means the hint stays visible, which is harmless.
    */
    void updateVirtualInputHint ([[maybe_unused]] const juce::StringArray& inputNames)
    {
        #if JUCE_WINDOWS
        static constexpr const char* kVirtualInputMarkers[] = {
            "CABLE Output",      // VB-CABLE
            "VB-Audio",          // VB-CABLE A/B, Hi-Fi Cable
            "VoiceMeeter Out",   // VoiceMeeter
            "Virtual Cable"
        };

        bool foundVirtualInput = false;

        for (const auto& name : inputNames)
        {
            for (const auto* marker : kVirtualInputMarkers)
            {
                if (name.containsIgnoreCase (marker))
                {
                    foundVirtualInput = true;
                    break;
                }
            }

            if (foundVirtualInput)
                break;
        }

        const bool shouldShow = ! foundVirtualInput;

        if (shouldShow != virtualInputHint.isVisible())
        {
            virtualInputHint.setVisible (shouldShow);
            getCableButton.setVisible   (shouldShow);
            resized();   // the hint occupies a layout row only while visible
        }
        #endif
    }

    void rebuildSampleRateCombo()
    {
        sampleRateCombo.clear (juce::dontSendNotification);

        auto* device      = deviceManager.getCurrentAudioDevice();
        const auto setup  = deviceManager.getAudioDeviceSetup();

        if (device != nullptr)
        {
            const auto rates = device->getAvailableSampleRates();
            int sel = 1;
            for (int i = 0; i < rates.size(); ++i)
            {
                const int hz = juce::roundToInt (rates[i]);
                const auto text = (hz % 1000 == 0)
                                  ? juce::String (hz / 1000) + " kHz"
                                  : juce::String (hz) + " Hz";
                sampleRateCombo.addItem (text, i + 1);
                if (juce::approximatelyEqual (rates[i], setup.sampleRate)) sel = i + 1;
            }
            sampleRateCombo.setSelectedId (sel, juce::dontSendNotification);
        }
        else
        {
            sampleRateCombo.addItem ("(no device)", 1);
            sampleRateCombo.setSelectedId (1, juce::dontSendNotification);
        }
    }

    void rebuildBufferSizeCombo()
    {
        bufferSizeCombo.clear (juce::dontSendNotification);

        auto* device     = deviceManager.getCurrentAudioDevice();
        const auto setup = deviceManager.getAudioDeviceSetup();

        if (device != nullptr)
        {
            const auto sizes = device->getAvailableBufferSizes();
            const double sr  = setup.sampleRate > 0.0 ? setup.sampleRate : 44100.0;
            int sel = 1;
            for (int i = 0; i < sizes.size(); ++i)
            {
                const double ms = (static_cast<double> (sizes[i]) / sr) * 1000.0;
                const auto text = juce::String (sizes[i])
                                  + " samples  ("
                                  + juce::String (ms, 1)
                                  + " ms)";
                bufferSizeCombo.addItem (text, i + 1);
                if (sizes[i] == setup.bufferSize) sel = i + 1;
            }
            bufferSizeCombo.setSelectedId (sel, juce::dontSendNotification);
        }
        else
        {
            bufferSizeCombo.addItem ("(no device)", 1);
            bufferSizeCombo.setSelectedId (1, juce::dontSendNotification);
        }
    }

    void commitAllSettings()
    {
        applyButton.setEnabled (false);

        // Snapshot combo state before the async hop (combos may be rebuilt afterwards)
        const auto typeName = deviceTypeCombo.getText();
        const auto inName   = inputDeviceCombo.getText();
        const auto outName  = outputDeviceCombo.getText();
        const int  rateId   = sampleRateCombo.getSelectedId();
        const int  bufId    = bufferSizeCombo.getSelectedId();
        // The chain, the bypass flags and the lane assignments are one value and
        // are snapshotted as one. Reading any of them live inside the lambda
        // below would apply an edit made after the click against a chain from
        // before it: lanes used to be read that way, so a lane change between
        // the click and the dispatch landed on the wrong plugin.
        const auto chain    = chainList.items;
        const auto bypass   = chainList.bypassed;
        const auto lanes    = chainList.lanes;

        // callAsync lets the button visually grey out before we block the message
        // thread with device restarts.
        juce::Component::SafePointer<PreferencesContentComponent> safe (this);
        juce::MessageManager::callAsync (
            [safe, typeName, inName, outName, rateId, bufId, chain, bypass, lanes]
            {
                if (safe == nullptr) return;
                auto& dm = safe->deviceManager;
                juce::Logger::writeToLog ("Preferences: Apply pressed");

                // 1. Device API type (may change which devices are available)
                try
                {
                    if (typeName.isNotEmpty() && typeName != dm.getCurrentAudioDeviceType())
                        dm.setCurrentAudioDeviceType (typeName, true);
                }
                catch (const std::exception& e)
                {
                    juce::Logger::writeToLog ("Preferences: setCurrentAudioDeviceType threw std::exception: "
                                              + juce::String (e.what()));
                }
                catch (...)
                {
                    juce::Logger::writeToLog ("Preferences: setCurrentAudioDeviceType threw unknown exception");
                }

                // 2. Input / output device names + sample rate + buffer size in one call
                auto setup = dm.getAudioDeviceSetup();
                auto* currentType = [&]() -> juce::AudioIODeviceType*
                {
                    const auto currentTypeName = dm.getCurrentAudioDeviceType();
                    for (auto* type : dm.getAvailableDeviceTypes())
                        if (type->getTypeName() == currentTypeName)
                            return type;
                    return nullptr;
                }();

                if (currentType != nullptr)
                {
                    const auto availableInputs  = currentType->getDeviceNames (true);
                    const auto availableOutputs = currentType->getDeviceNames (false);

                    if (inName.isNotEmpty() && ! inName.startsWith ("(") && availableInputs.contains (inName))
                        setup.inputDeviceName = inName;
                    if (outName.isNotEmpty() && ! outName.startsWith ("(") && availableOutputs.contains (outName))
                        setup.outputDeviceName = outName;
                }

                auto* device = dm.getCurrentAudioDevice();
                if (device != nullptr)
                {
                    const auto rates = device->getAvailableSampleRates();
                    const int  ri    = rateId - 1;
                    if (ri >= 0 && ri < rates.size())
                        setup.sampleRate = rates[ri];

                    const auto sizes = device->getAvailableBufferSizes();
                    const int  bi    = bufId - 1;
                    if (bi >= 0 && bi < sizes.size())
                        setup.bufferSize = sizes[bi];
                }
                setup.useDefaultInputChannels  = true;
                setup.useDefaultOutputChannels = true;
                try
                {
                    if (auto error = dm.setAudioDeviceSetup (setup, true); error.isNotEmpty())
                        juce::Logger::writeToLog ("Preferences: setAudioDeviceSetup error: " + error);
                }
                catch (const std::exception& e)
                {
                    juce::Logger::writeToLog ("Preferences: setAudioDeviceSetup threw std::exception: "
                                              + juce::String (e.what()));
                }
                catch (...)
                {
                    juce::Logger::writeToLog ("Preferences: setAudioDeviceSetup threw unknown exception");
                }

                // 3. Plugin chain + bypass states
                if (safe->onApplyFn) safe->onApplyFn (chain, bypass, lanes);

                // 4. Re-enable Apply now that all restarts are complete
                if (safe != nullptr)
                    safe->applyButton.setEnabled (true);
            });
    }

    //--------------------------------------------------------------------------
    /** Runs an audio file through this chain and writes the result.

        The render builds its own graph rather than borrowing the live one, so a
        test can never disturb what is currently processing audio, and it writes
        no settings and no plugin state. See OfflineRender.hpp for why that
        matters.
    */
    void runChainTest()
    {
        chainTestChooser = std::make_unique<juce::FileChooser> (
            "Choose an audio file to run through the chain",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory),
            "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg");

        juce::Component::SafePointer<PreferencesContentComponent> safe (this);

        chainTestChooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe] (const juce::FileChooser& fc)
            {
                if (safe == nullptr) return;

                const auto input = fc.getResult();
                if (input == juce::File() || ! input.existsAsFile()) return;

                safe->chooseChainTestOutput (input);
            });
    }

    void chooseChainTestOutput (const juce::File& input)
    {
        chainTestChooser = std::make_unique<juce::FileChooser> (
            "Choose a folder for the rendered result", input.getParentDirectory());

        juce::Component::SafePointer<PreferencesContentComponent> safe (this);

        chainTestChooser->launchAsync (
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectDirectories,
            [safe, input] (const juce::FileChooser& fc)
            {
                if (safe == nullptr) return;

                const auto folder = fc.getResult();
                if (folder == juce::File() || ! folder.isDirectory()) return;

                safe->startChainTest (input, folder);
            });
    }

    void startChainTest (const juce::File& input, const juce::File& folder)
    {
        if (chainTestInFlight)
        {
            setApplyFeedback ("Chain test already running");
            return;
        }

        const auto output = folder.getChildFile (input.getFileNameWithoutExtension()
                                                 + "-lighthost.wav");

        setApplyFeedback ("Rendering...");
        chainTestButton.setEnabled (false);
        chainTestInFlight = true;

        auto* settings = getAppProperties().getUserSettings();
        const auto stateDir = lighthost::state::directoryFor (settings->getFile());

        // The render stays on the message thread and keeps this window alive by
        // pumping the dispatch loop while pacing waits. It is paced to real time,
        // so a one-minute take takes a minute, and blocking for that long is not
        // acceptable -- but neither was the obvious fix.
        //
        // Rendering on a background thread looked tidier and was not. Creating a
        // plugin instance off the message thread posts an async message *to* the
        // message thread and blocks on it with no timeout, so quitting mid-render
        // hung that thread forever while it still held live plugin instances.
        // It also read a settings object whose fallback set the app frees during
        // shutdown, and logged through a logger shutdown deletes. Pumping the
        // loop keeps all three on the thread that owns them.
        //
        // Pumping does mean re-entrancy is possible -- the user can click things
        // mid-render -- which is what chainTestInFlight guards. Disabling the
        // button is not enough, because closing and reopening Preferences builds
        // a new one that is enabled.
        juce::Component::SafePointer<PreferencesContentComponent> safe (this);

        juce::MessageManager::callAsync ([safe, input, output, stateDir]
        {
            if (safe == nullptr)
            {
                chainTestInFlight = false;
                return;
            }

            auto* props = getAppProperties().getUserSettings();

            const auto result = lighthost::render::renderFile (
                *props, stateDir, input, output, 480, {}, true, {},
                [] (int ms)
                {
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
                });

            chainTestInFlight = false;

            if (safe == nullptr)
                return;

            safe->chainTestButton.setEnabled (true);

            if (! result.ok)
            {
                safe->setApplyFeedback ("Render failed");
                safe->setStatusMessage ("Chain test failed: " + result.message);
                return;
            }

            // A render that could not keep pace measured a starved plugin, which
            // is worse than useless -- it looks like a result. Say so here rather
            // than only in the log.
            //
            // pacingAbandoned comes first because it is the worse case and the
            // one blocksBehind cannot see: the wait stopped taking effect, so
            // audio time could no longer fall behind and the counter stayed at
            // zero for a render that was not paced at all.
            if (result.pacingAbandoned)
            {
                safe->setApplyFeedback ("Rendered, but pacing stopped");
                safe->setStatusMessage ("Chain test stopped being paced part way through, so "
                                        "the rest of it ran faster than real time and any "
                                        "plugin doing background inference was starved. "
                                        "Treat the output as unreliable and run it again.");
            }
            else if (result.pluginsStateNotRestored > 0)
            {
                safe->setApplyFeedback ("Rendered at factory defaults");
                safe->setStatusMessage (juce::String (result.pluginsStateNotRestored)
                                        + " plugin(s) rejected their saved state, so this "
                                          "render is not of your saved configuration.");
            }
            else if (result.blocksBehind > 0)
            {
                safe->setApplyFeedback ("Rendered, but fell behind real time");
                safe->setStatusMessage ("Chain test fell behind on "
                                        + juce::String (result.blocksBehind) + " of "
                                        + juce::String (result.blocksTotal)
                                        + " blocks - a plugin doing background "
                                          "inference was starved, so treat the "
                                          "output as unreliable");
            }
            else
            {
                safe->setApplyFeedback ("Rendered - " + juce::String (result.declaredLatency)
                                        + " samples latency");
            }

            output.revealToUser();
        });
    }

    void updateChannelLabels()
    {
        auto* device = deviceManager.getCurrentAudioDevice();

        const int numIn  = device ? juce::jmin (device->getInputChannelNames().size(),  2) : 2;
        const int numOut = device ? juce::jmin (device->getOutputChannelNames().size(), 2) : 2;

        inputChannelLabel.setText  (numIn  == 1 ? "Mono (Ch 1)"    : "Stereo (Ch 1-2)",
                                    juce::dontSendNotification);
        outputChannelLabel.setText (numOut == 1 ? "Mono (Ch 1)"    : "Stereo (Ch 1-2)",
                                    juce::dontSendNotification);
    }

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (source == &deviceManager)
        {
            rebuildDeviceCombos();
            updateLatencyDisplay();
        }
    }

    /** Chain latency, device latency, and the sum -- which is the only one of
        the three the user actually experiences.

        Event driven rather than polled: the device broadcasts its changes, and
        IconMenu refreshes this panel when a plugin re-declares its latency
        (which smartChain does when its latency mode is switched). A timer would
        be simpler and would burn cycles forever to catch an event that arrives
        perhaps twice a session.
    */
    void updateLatencyDisplay()
    {
        const auto chainSamples = chainLatencyFn ? chainLatencyFn() : 0;

        auto* device = deviceManager.getCurrentAudioDevice();
        const auto rate = device != nullptr ? device->getCurrentSampleRate() : 0.0;

        if (rate <= 0.0)
        {
            latencyValueLabel.setText ("no audio device", juce::dontSendNotification);
            return;
        }

        const auto deviceSamples = device->getInputLatencyInSamples()
                                 + device->getOutputLatencyInSamples();

        const auto ms = [rate] (int samples)
        {
            return juce::String (samples * 1000.0 / rate, 1);
        };

        latencyValueLabel.setText (
            ms (chainSamples) + " ms plugins + " + ms (deviceSamples)
                + " ms device  =  " + ms (chainSamples + deviceSamples) + " ms",
            juce::dontSendNotification);
    }

    /** Flips the toggle and tells whoever is listening. The window does the
        widening and the panel wiring, because only it owns both halves.
    */
    void toggleSignalView()
    {
        signalViewToggle.setToggled (! signalViewToggle.isToggled());

        if (onSignalViewToggledFn)
            onSignalViewToggledFn (signalViewToggle.isToggled());
    }

    void updateChainListHeight()
    {
        if (chainViewport.getWidth() <= 0) return;
        const int preferred = chainList.getPreferredHeight();
        const int minH      = chainViewport.getHeight();
        chainList.setSize (
            chainViewport.getWidth() - chainViewport.getScrollBarThickness(),
            juce::jmax (minH, preferred));
    }

    void showAddPluginMenu()
    {
        // Build set of already-staged plugins for greying-out
        std::set<juce::String> active;
        for (const auto& p : chainList.items)
            active.insert (p.fileOrIdentifier + p.pluginFormatName + p.name);

        // Sort available plugins: manufacturer then name
        auto types = knownPlugins.getTypes();
        std::sort (types.begin(), types.end(),
                   [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
                   {
                       const int mfr = a.manufacturerName.compareIgnoreCase (b.manufacturerName);
                       return mfr != 0 ? mfr < 0 : a.name.compareIgnoreCase (b.name) < 0;
                   });

        // Build popup with manufacturer sub-menus.
        //
        // The group is flushed on "a group was started", not on "its name is not
        // empty". Sorting puts plugins with no manufacturer first, so the old
        // condition never flushed them: the submenu they had accumulated into was
        // cleared when the first named manufacturer arrived, and every one of
        // those plugins vanished from the menu. Blank manufacturers are common in
        // VST2, which this host still supports. If every plugin had a blank
        // manufacturer, the menu came up empty.
        const auto labelFor = [] (const juce::String& manufacturer)
        {
            return manufacturer.isNotEmpty() ? manufacturer
                                             : juce::String ("(no manufacturer)");
        };

        juce::PopupMenu menu;
        juce::PopupMenu subMenu;
        juce::String    currentMfr;
        bool            groupStarted = false;
        int id = 1;

        for (const auto& pd : types)
        {
            if (! groupStarted || pd.manufacturerName != currentMfr)
            {
                if (groupStarted)
                    menu.addSubMenu (labelFor (currentMfr), subMenu);

                subMenu.clear();
                currentMfr   = pd.manufacturerName;
                groupStarted = true;
            }

            // Ids are 1-based indices into `types`, which the callback below
            // relies on, so this counter must advance once per plugin in order.
            const auto key = pd.fileOrIdentifier + pd.pluginFormatName + pd.name;
            subMenu.addItem (id++, pd.name, active.count (key) == 0);
        }

        if (groupStarted)
            menu.addSubMenu (labelFor (currentMfr), subMenu);

        if (types.isEmpty())
            menu.addItem (1, "(no plugins scanned)", false);

        juce::Component::SafePointer<PreferencesContentComponent> safe (this);
        menu.showMenuAsync (
            juce::PopupMenu::Options{}.withTargetComponent (addPluginButton),
            [safe, types = std::move (types)] (int result) mutable
            {
                if (safe == nullptr) return;
                if (result <= 0 || result > static_cast<int> (types.size())) return;
                safe->chainList.items.push_back (types[static_cast<size_t> (result - 1)]);
                safe->chainList.bypassed.push_back (false);
                safe->chainList.syncBypassedSize();  // extends lanes too — keeps the trio in lockstep
                safe->updateChainListHeight();
                safe->chainList.repaint();
            });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesContentComponent)
};

//==============================================================================
// Holds the panel and scrolls it when the window is shorter than the layout needs.
//
// resized() lays out a fixed stack with removeFromTop, so a window shorter than
// the stack does not compress it: the sections at the end get whatever is left,
// which is nothing, and the Apply button disappears. Raising the window's minimum
// height fixes that until the next section is added and the arithmetic has to be
// redone. Scrolling retires the problem instead.
//==============================================================================
class PreferencesPanelViewport final : public juce::Viewport
{
public:
    explicit PreferencesPanelViewport (PreferencesContentComponent* panelToOwn)
        : panel (panelToOwn)
    {
        setViewedComponent (panel, true);   // the viewport owns it
        setScrollBarsShown (true, false);
    }

    void resized() override
    {
        juce::Viewport::resized();

        if (panel == nullptr)
            return;

        // The scrollbar's width is decided from the height the panel wants, which
        // does not depend on the width, so this cannot oscillate.
        const int preferred = panel->getPreferredHeight();
        const bool needsScrollBar = preferred > getHeight();
        const int width = getWidth() - (needsScrollBar ? getScrollBarThickness() : 0);

        panel->setSize (juce::jmax (200, width), juce::jmax (getHeight(), preferred));
    }

    PreferencesContentComponent* panel = nullptr;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesPanelViewport)
};

//==============================================================================
// PreferencesShell
//
// The window's content: the scrolling panel, and the signal view beside it.
//
// The side panel cannot live inside the scrolling panel. That viewport clamps
// its child to its own width, so a column added in there would steal width from
// the main layout rather than extend the window, and it would scroll away
// vertically with the rest of the content.
//
// Everything that used to reach the panel by casting the window's content to
// PreferencesPanelViewport now comes through here instead. Those casts fail
// silently -- they return nullptr and the caller does nothing -- so they are the
// one thing that had to change in lockstep with this class existing.
//==============================================================================
class PreferencesShell final : public juce::Component
{
public:
    PreferencesShell (PreferencesContentComponent* panelToOwn)
        : viewport (panelToOwn)
    {
        addAndMakeVisible (viewport);
        addChildComponent (signalView);   // not visible until it is opened
    }

    [[nodiscard]] PreferencesContentComponent* panel() const noexcept { return viewport.panel; }
    [[nodiscard]] SignalViewPanel& getSignalView() noexcept { return signalView; }

    void setSignalViewVisible (bool shouldShow)
    {
        signalView.setVisible (shouldShow);
        resized();
    }

    [[nodiscard]] bool isSignalViewVisible() const { return signalView.isVisible(); }

    void resized() override
    {
        auto area = getLocalBounds();

        // The side panel takes a fixed width off the right, so the main column
        // keeps the width it had before the window grew.
        if (signalView.isVisible())
            signalView.setBounds (area.removeFromRight (SignalViewPanel::kWidth));

        viewport.setBounds (area);
    }

private:
    PreferencesPanelViewport viewport;
    SignalViewPanel signalView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesShell)
};

//==============================================================================
// PreferencesWindow
//==============================================================================
PreferencesWindow::PreferencesWindow (
    juce::AudioDeviceManager& deviceManager,
    juce::KnownPluginList& knownPlugins,
    const std::vector<juce::PluginDescription>& activeChain,
    const std::vector<bool>& bypassStates,
    const std::vector<int>& laneStates,
    LaneTrim laneTrim,
    std::function<void (const std::vector<juce::PluginDescription>&,
                        const std::vector<bool>&,
                        const std::vector<int>&)> onApply,
    std::function<void (const juce::PluginDescription&)> onEditPlugin,
    std::function<int()> chainLatencySamples,
    lighthost::metering::Meter* inputMeter,
    lighthost::metering::Meter* outputMeter,
    std::function<void (bool enabled)> onSignalViewToggled,
    std::function<lighthost::metering::Meter* (int index)> probeMeterAt,
    std::function<void()> onClose)
    : DocumentWindow ("Preferences",
                      juce::LookAndFeel::getDefaultLookAndFeel()
                          .findColour (juce::ResizableWindow::backgroundColourId),
                      DocumentWindow::minimiseButton | DocumentWindow::closeButton),
      onCloseFn (std::move (onClose))
{
    chainLatencyFn = chainLatencySamples;

    auto* content = new PreferencesContentComponent (
        deviceManager, knownPlugins, activeChain, bypassStates, laneStates,
        std::move (laneTrim), std::move (onApply), std::move (onEditPlugin),
        std::move (chainLatencySamples), inputMeter, outputMeter,
        // The host is told first so the probes exist, then the column is filled
        // from them. Doing it the other way round would read every meter before
        // it had been created.
        [this, onSignalViewToggled = std::move (onSignalViewToggled)] (bool enabled)
        {
            if (onSignalViewToggled)
                onSignalViewToggled (enabled);

            setSignalViewOpen (enabled);
        },
        std::move (probeMeterAt));

    // Height budget. The authority is fixedLayoutHeight(), not this comment --
    // which had drifted 34px out of date within one release of being written,
    // when the Latency row landed and was added to the arithmetic but not to the
    // prose. What follows is a sanity check on that function, not a second
    // source of truth.
    //
    // Above the chain: 10 pad + 62 INPUT + 26 input meter + 34 virtual-input hint
    // when shown + 28 chain label = 160. Below it: 40 Add Plugin + 62 LANE TRIM +
    // 62 OUTPUT + 26 output meter + 164 DEVICE SETTINGS including Latency + 50
    // buttons and pad = 404. With the chain viewport at its 80px minimum that is
    // 644 of content, so the default below sits just under the preferred height
    // and the viewport scrolls -- which is what it is for, and why adding a row
    // no longer means re-deriving a window size.
    constexpr int kDefaultWidth  = 520;
    constexpr int kDefaultHeight = 650;

    // The panel lives inside a viewport that never sizes it below the height its
    // layout needs, so a short window scrolls rather than losing its lower
    // sections. The minimum below is now about comfort, not correctness.
    auto* shell = new PreferencesShell (content);
    shell->setSize (kDefaultWidth, kDefaultHeight);

    setContentOwned (shell, true);
    setUsingNativeTitleBar (true);
    setResizable (true, false);

    // Same reasoning as the plugin list window: the old 900x1100 ceiling was a
    // constant, not a layout constraint. The panel scrolls inside a viewport, so
    // a taller window simply shows more of it.
    const auto desktop = Desktop::getInstance().getDisplays().getTotalBounds (true);
    setResizeLimits (440, 400,
                     jmax (440, desktop.getWidth()),
                     jmax (400, desktop.getHeight()));
    centreWithSize (kDefaultWidth, kDefaultHeight);
    setVisible (true);
}

PreferencesWindow::~PreferencesWindow() = default;

void PreferencesWindow::closeButtonPressed()
{
    if (onCloseFn) onCloseFn();
}

void PreferencesWindow::refreshPluginChain (const std::vector<juce::PluginDescription>& chain,
                                             const std::vector<bool>& bypassStates,
                                             const std::vector<int>& laneStates)
{
    if (auto* panel = contentPanel())
    {
        panel->setChain (chain, bypassStates, laneStates);

        // The chain changed, so the probes were recreated and the meters the
        // signal view holds have moved. Rebuild the column from the new ones.
        if (auto* shell = dynamic_cast<PreferencesShell*> (getContentComponent()))
            if (shell->isSignalViewVisible())
                panel->refreshSignalView (shell->getSignalView(), latencySamples());
    }

}

void PreferencesWindow::setStatusMessage (const juce::String& message)
{
    if (auto* panel = contentPanel())
        panel->setStatusMessage (message);
}

void PreferencesWindow::setApplyFeedback (const juce::String& message)
{
    if (auto* panel = contentPanel())
        panel->setApplyFeedback (message);
}

PreferencesContentComponent* PreferencesWindow::contentPanel() const
{
    if (auto* shell = dynamic_cast<PreferencesShell*> (getContentComponent()))
        return shell->panel();

    return nullptr;
}

int PreferencesWindow::latencySamples() const
{
    return chainLatencyFn ? chainLatencyFn() : 0;
}

void PreferencesWindow::setSignalViewOpen (bool shouldBeOpen)
{
    auto* shell = dynamic_cast<PreferencesShell*> (getContentComponent());

    if (shell == nullptr || shell->isSignalViewVisible() == shouldBeOpen)
        return;

    if (shouldBeOpen)
    {
        // Remembered, not derived. Subtracting the panel width on the way back
        // would lose any resizing the user did while it was open.
        widthBeforeSignalView = getWidth();

        if (auto* panel = shell->panel())
            panel->refreshSignalView (shell->getSignalView(), latencySamples());
    }

    shell->setSignalViewVisible (shouldBeOpen);

    // setFullScreen first, because a programmatic resize fights a window the OS
    // has maximised and the result is a window that ignores the toggle.
    if (isFullScreen())
        setFullScreen (false);

    // The floor comes from the constrainer setResizeLimits installed, rather
    // than a second copy of the number.
    const auto minWidth = getConstrainer() != nullptr ? getConstrainer()->getMinimumWidth()
                                                      : getWidth();

    const auto target = shouldBeOpen
                            ? getWidth() + SignalViewPanel::kWidth
                            : juce::jmax (minWidth, widthBeforeSignalView);

    setSize (target, getHeight());
}
