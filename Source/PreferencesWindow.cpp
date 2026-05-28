#include "PreferencesWindow.h"
#include <set>

using namespace juce;

juce::ApplicationProperties& getAppProperties();

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
//   [checkbox]  [plugin name]  [Edit]  [drag handle ☰]
//
// Checkbox: active (filled blue + tick) = plugin active; unchecked = bypassed.
// Drag handle: click-and-drag anywhere in the row (outside checkbox/Edit) to
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
                g.setColour (juce::Colour (0xff4a9eff));
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
            const auto editRect = getEditButtonArea (i);
            g.setColour (bg.darker (0.18f));
            g.fillRoundedRectangle (editRect.toFloat(), 3.0f);
            g.setColour (txt.withAlpha (0.70f));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (11.5f)));
            g.drawText ("Edit", editRect, juce::Justification::centred);
            
            // ── Lane button ────────────────────────────────────────────────
            const auto laneRect = getLaneButtonArea (i);
            g.setColour (bg.darker (0.05f));
            g.fillRoundedRectangle (laneRect.toFloat(), 3.0f);
            g.setColour (txt.withAlpha (0.85f));
            int ln = (static_cast<size_t>(i) < lanes.size()) ? lanes[static_cast<size_t>(i)] : 0;
            g.drawText ("Lane " + juce::String(ln), laneRect, juce::Justification::centred);

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

        // Lane button
        if (getLaneButtonArea(row).contains(e.getPosition()))
        {
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
                    if (safe == nullptr || result == 0) return;
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

        // Edit button
        if (getEditButtonArea (row).contains (e.getPosition()))
        {
            if (onEditClicked) onEditClicked (row);
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
        if (dragSourceRow < 0) return;
        const int newDropLine = juce::jlimit (0, static_cast<int> (items.size()),
                                              (e.y + kRowHeight / 2) / kRowHeight);
        if (newDropLine != dropLine) { dropLine = newDropLine; repaint(); }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
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
    int dragSourceRow = -1;
    int dragOffsetY   = 0;
    int dropLine      = -1;

    [[nodiscard]] juce::Rectangle<int> getCheckboxArea (int row) const noexcept
    {
        const int cy = row * kRowHeight + kRowHeight / 2;
        return { 8, cy - 9, 18, 18 };
    }

        [[nodiscard]] juce::Rectangle<int> getNameArea (int row) const noexcept
    {
        return { 34, row * kRowHeight, getWidth() - 34 - 118 - 26, kRowHeight };
    }

    [[nodiscard]] juce::Rectangle<int> getEditButtonArea (int row) const noexcept
    {
        return { getWidth() - 80, row * kRowHeight + 7, 52, kRowHeight - 14 };
    }

    [[nodiscard]] juce::Rectangle<int> getLaneButtonArea (int row) const noexcept
    {
        return { getWidth() - 140, row * kRowHeight + 7, 52, kRowHeight - 14 };
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
        std::function<void (const std::vector<juce::PluginDescription>&,
                            const std::vector<bool>&,
                            const std::vector<int>&)> onApply,
        std::function<void (const juce::PluginDescription&)> onEditPlugin)
        : deviceManager (dm),
          knownPlugins   (knownPlugins_),
          onApplyFn      (std::move (onApply)),
          onEditPluginFn (std::move (onEditPlugin))
    {
        // ── INPUT section ─────────────────────────────────────────────────────
        addAndMakeVisible (inputSectionLabel);
        addAndMakeVisible (inputDeviceCombo);
        addAndMakeVisible (inputChannelLabel);
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

        // Device combos are staged — no immediate apply on change.
        // ChangeListener below keeps them in sync with external device events.

        deviceManager.addChangeListener (this);
        rebuildDeviceCombos();
    }

    ~PreferencesContentComponent() override
    {
        deviceManager.removeChangeListener (this);
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

    void resized() override
    {
        constexpr int kPad    = 10;
        constexpr int kSectH  = 22;
        constexpr int kRowH   = 28;
        constexpr int kGap    = 6;
        constexpr int kBtnH   = 40;
        constexpr int kMinChainH = 80;

        // Fixed-height sections below/above the chain viewport
        constexpr int kFixedAboveChain = kPad
            + kSectH + kGap + kRowH + kGap     // INPUT
            + kSectH + kGap;                    // AUDIO CHAIN label
        constexpr int kFixedBelowChain = kGap + kRowH + kGap           // Add Plugin row
            + kSectH + kGap + kRowH + kGap                          // OUTPUT
            + kSectH + kGap + kRowH + kGap + kRowH + kGap + kRowH + kGap  // DEVICE SETTINGS (API + rate + buffer)
            + kBtnH + kPad;

        const int chainViewH = juce::jmax (
            kMinChainH,
            getHeight() - kFixedAboveChain - kFixedBelowChain);

        auto area = getLocalBounds().reduced (kPad, kPad);

        // ── INPUT ──────────────────────────────────────────────────────────────
        inputSectionLabel.setBounds (area.removeFromTop (kSectH));
        area.removeFromTop (kGap);
        {
            auto row = area.removeFromTop (kRowH);
            inputChannelLabel.setBounds (row.removeFromRight (130));
            inputDeviceCombo.setBounds  (row.reduced (0, 2));
        }
        area.removeFromTop (kGap);

        // ── AUDIO CHAIN ────────────────────────────────────────────────────────
        chainSectionLabel.setBounds (area.removeFromTop (kSectH));
        area.removeFromTop (kGap);
        chainViewport.setBounds (area.removeFromTop (chainViewH));
        updateChainListHeight();
        area.removeFromTop (kGap);
        addPluginButton.setBounds (
            area.removeFromTop (kRowH).removeFromRight (120).reduced (0, 3));
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
            applyButton.setBounds   (row.removeFromRight (90).reduced (4, 6));
            versionLabel.setBounds  (row.removeFromLeft (120).reduced (4, 6));
        }
    }

private:
    juce::AudioDeviceManager& deviceManager;
    juce::KnownPluginList&    knownPlugins;

    std::function<void (const std::vector<juce::PluginDescription>&,
                        const std::vector<bool>&,
                        const std::vector<int>&)>    onApplyFn;
    std::function<void (const juce::PluginDescription&)> onEditPluginFn;

    // INPUT
    SectionLabel   inputSectionLabel   { "  INPUT" };
    juce::ComboBox inputDeviceCombo;
    juce::Label    inputChannelLabel;

    // AUDIO CHAIN
    SectionLabel            chainSectionLabel  { "  AUDIO CHAIN" };
    AudioChainListComponent chainList;
    juce::Viewport          chainViewport;
    juce::TextButton        addPluginButton;

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

    // Apply button + version label
    juce::TextButton applyButton;
    juce::Label      versionLabel;


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
        const auto chain    = chainList.items;
        const auto bypass   = chainList.bypassed;

        // callAsync lets the button visually grey out before we block the message
        // thread with device restarts.
        juce::Component::SafePointer<PreferencesContentComponent> safe (this);
        juce::MessageManager::callAsync (
            [safe, typeName, inName, outName, rateId, bufId, chain, bypass]
            {
                if (safe == nullptr) return;
                auto& dm = safe->deviceManager;
                juce::Logger::writeToLog ("Preferences: Apply pressed");

                // 1. Device API type (may change which devices are available)
                if (typeName.isNotEmpty() && typeName != dm.getCurrentAudioDeviceType())
                    dm.setCurrentAudioDeviceType (typeName, true);

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
                if (auto error = dm.setAudioDeviceSetup (setup, true); error.isNotEmpty())
                    juce::Logger::writeToLog ("Preferences: setAudioDeviceSetup error: " + error);

                // 3. Plugin chain + bypass states
                if (safe->onApplyFn) safe->onApplyFn (chain, bypass, safe->chainList.lanes);

                // 4. Re-enable Apply now that all restarts are complete
                if (safe != nullptr)
                    safe->applyButton.setEnabled (true);
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

        // Build popup with manufacturer sub-menus
        juce::PopupMenu menu;
        juce::String    lastMfr;
        juce::PopupMenu subMenu;
        int id = 1;

        for (const auto& pd : types)
        {
            if (pd.manufacturerName != lastMfr)
            {
                if (lastMfr.isNotEmpty())
                    menu.addSubMenu (lastMfr, subMenu);
                subMenu.clear();
                lastMfr = pd.manufacturerName;
            }
            const auto key = pd.fileOrIdentifier + pd.pluginFormatName + pd.name;
            subMenu.addItem (id++, pd.name, active.count (key) == 0);
        }
        if (lastMfr.isNotEmpty())
            menu.addSubMenu (lastMfr, subMenu);

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
// PreferencesWindow
//==============================================================================
PreferencesWindow::PreferencesWindow (
    juce::AudioDeviceManager& deviceManager,
    juce::KnownPluginList& knownPlugins,
    const std::vector<juce::PluginDescription>& activeChain,
    const std::vector<bool>& bypassStates,
    const std::vector<int>& laneStates,
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
        std::move (onApply), std::move (onEditPlugin));

    content->setSize (520, 560);
    setContentOwned (content, true);
    setUsingNativeTitleBar (true);
    setResizable (true, false);
    setResizeLimits (440, 500, 900, 1000);
    centreWithSize (520, 560);
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
    if (auto* content = dynamic_cast<PreferencesContentComponent*> (getContentComponent()))
        content->setChain (chain, bypassStates, laneStates);
}
