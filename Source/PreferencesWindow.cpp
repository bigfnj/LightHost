#include "PreferencesWindow.h"
#include "GainProcessor.hpp"
#include "LookAndFeel.hpp"
#include "OfflineRender.hpp"
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

            juce::PopupMenu m;
            m.addItem(1, "Lane 0");
            m.addItem(2, "Lane 1");
            m.addItem(3, "Lane 2");
            m.addItem(4, "Lane 3");
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
        dragOffsetY   = e.y - row * kRowHeight;
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
    int dragOffsetY   = 0;
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
        std::function<void (const juce::PluginDescription&)> onEditPlugin)
        : deviceManager (dm),
          knownPlugins   (knownPlugins_),
          laneTrim       (std::move (laneTrimIn)),
          onApplyFn      (std::move (onApply)),
          onEditPluginFn (std::move (onEditPlugin))
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
    }

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
            + hintH                             // virtual-input hint, when shown
            + kSectH + kGap;                    // AUDIO CHAIN label

        const int belowChain = kGap + kRowH + kGap           // Add Plugin row
            + kSectH + kGap + kRowH + kGap                    // LANE TRIM
            + kSectH + kGap + kRowH + kGap                    // OUTPUT
            + kSectH + kGap + kRowH + kGap + kRowH + kGap + kRowH + kGap  // DEVICE SETTINGS
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
        chainSectionLabel.setBounds (area.removeFromTop (kSectH));
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
        const auto output = folder.getChildFile (input.getFileNameWithoutExtension()
                                                 + "-lighthost.wav");

        setApplyFeedback ("Rendering...");
        chainTestButton.setEnabled (false);

        // callAsync for the same reason Apply uses it: the label repaints before
        // the render blocks the message thread. Rendering runs far faster than
        // real time, so a voice take is a moment -- but a very long file will
        // make this window briefly unresponsive, which is the honest trade for
        // not instantiating plugins off the message thread.
        juce::Component::SafePointer<PreferencesContentComponent> safe (this);

        juce::MessageManager::callAsync ([safe, input, output]
        {
            if (safe == nullptr) return;

            auto* settings = getAppProperties().getUserSettings();
            const auto settingsFile = settings->getFile();
            const auto stateDir = settingsFile.getSiblingFile (
                settingsFile.getFileNameWithoutExtension() + ".state");

            const auto result =
                lighthost::render::renderFile (*settings, stateDir, input, output);

            safe->chainTestButton.setEnabled (true);

            if (result.ok)
            {
                safe->setApplyFeedback ("Rendered - " + juce::String (result.declaredLatency)
                                        + " samples latency");
                output.revealToUser();
            }
            else
            {
                safe->setApplyFeedback ("Render failed");
                safe->setStatusMessage ("Chain test failed: " + result.message);
            }
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
            rebuildDeviceCombos();
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
    std::function<void()> onClose)
    : DocumentWindow ("Preferences",
                      juce::LookAndFeel::getDefaultLookAndFeel()
                          .findColour (juce::ResizableWindow::backgroundColourId),
                      DocumentWindow::minimiseButton | DocumentWindow::closeButton),
      onCloseFn (std::move (onClose))
{
    auto* content = new PreferencesContentComponent (
        deviceManager, knownPlugins, activeChain, bypassStates, laneStates,
        std::move (laneTrim), std::move (onApply), std::move (onEditPlugin));

    // Height budget, from the constants in resized(). Above the chain: 10 pad +
    // 62 INPUT + 34 virtual-input hint when shown + 28 chain label = 134. Below
    // it: 40 Add Plugin + 62 LANE TRIM + 62 OUTPUT + 130 DEVICE SETTINGS + 50
    // buttons and pad = 344. With the chain viewport at its 80px minimum that is
    // 558 of content, so the default leaves the chain some room beyond the
    // minimum and the smallest allowed size still fits everything.
    //
    // These numbers were too small before the LANE TRIM section was added: with
    // the virtual-input hint showing, the default window was already about 28px
    // short and the sections at the bottom were silently squeezed to nothing.
    constexpr int kDefaultWidth  = 520;
    constexpr int kDefaultHeight = 650;

    // The panel lives inside a viewport that never sizes it below the height its
    // layout needs, so a short window scrolls rather than losing its lower
    // sections. The minimum below is now about comfort, not correctness.
    auto* scroller = new PreferencesPanelViewport (content);
    scroller->setSize (kDefaultWidth, kDefaultHeight);

    setContentOwned (scroller, true);
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
    // The window's content is the scrolling viewport now, and the panel is inside
    // it, so this reaches one level further down than it used to.
    if (auto* scroller = dynamic_cast<PreferencesPanelViewport*> (getContentComponent()))
        if (scroller->panel != nullptr)
            scroller->panel->setChain (chain, bypassStates, laneStates);
}

void PreferencesWindow::setStatusMessage (const juce::String& message)
{
    if (auto* scroller = dynamic_cast<PreferencesPanelViewport*> (getContentComponent()))
        if (scroller->panel != nullptr)
            scroller->panel->setStatusMessage (message);
}

void PreferencesWindow::setApplyFeedback (const juce::String& message)
{
    if (auto* scroller = dynamic_cast<PreferencesPanelViewport*> (getContentComponent()))
        if (scroller->panel != nullptr)
            scroller->panel->setApplyFeedback (message);
}
