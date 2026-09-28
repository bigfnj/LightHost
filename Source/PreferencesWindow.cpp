#include "PreferencesWindow.h"
#include "AudioChainList.hpp"
#include "GainProcessor.hpp"
#include "HostServices.hpp"
#include "LookAndFeel.hpp"
#include "OfflineRender.hpp"
#include "LoadPolicy.hpp"
#include "PluginChainStore.hpp"
#include "PreferencesLayout.hpp"
#include "SignalMetering.hpp"
#include "SignalView.hpp"
#include "UiMetrics.hpp"
#include "VisibilityTimers.hpp"
#include "Lanes.hpp"
#include <set>

using namespace juce;

namespace metrics = lighthost::ui::metrics;
namespace prefs   = lighthost::ui::prefs;

// The chain list moved to Source/AudioChainList.hpp so it could be unit tested;
// see that file's banner. The two names this file uses most are pulled in here
// rather than qualified at every use, in a file that already opens with
// `using namespace juce`. The other `lighthost::ui::` uses stay qualified --
// they are one-offs, and shortening them would mean more of these lines rather
// than fewer.
using lighthost::ui::AudioChainListComponent;
using lighthost::ui::ChainRows;

// The signal view -- the two device meters, the tap column and the rules that
// fill it -- moved to Source/SignalView.hpp for the same reason and on the same
// terms; see that file's banner. Named here for the same reason as above: these
// two are members of the classes below and appear at nearly every use site.
using lighthost::ui::SignalMeter;
using lighthost::ui::SignalViewPanel;

// getAppProperties and getLogFile come from HostServices.hpp above. This file
// used to declare both itself, which is how getLogFile ended up with a
// declaration nothing checked against its definition.

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

//==============================================================================
// The audio-load readout.
//
// A component rather than a pair of juce::Labels because it needs three things
// a Label does not give: a timer, a colour that follows severity, and a
// tooltip.
//
// POLLED, WHICH THE ROW ABOVE IT DELIBERATELY IS NOT. updateLatencyDisplay is
// event-driven and says in as many words that a timer would burn cycles
// forever to catch an event arriving twice a session. This number is the
// opposite: it moves continuously and nothing broadcasts it, so there is
// nothing to be driven by. It is polled at the rate the figure behind it
// changes -- not at meter rate, which would be 24 wasted wakeups a second --
// and only while it is on screen.
//==============================================================================
class LoadReadout final : public juce::Component,
                          public juce::SettableTooltipClient,
                          private juce::Timer,
                          private lighthost::ui::VisibilityDrivenTimer
{
public:
    LoadReadout()
    {
        setTooltip ("How much of each audio block the chain is using, the highest it has "
                    "reached since this device was opened, and how many blocks missed "
                    "their deadline.\n\n"
                    "A dropout the driver reported is one the hardware noticed. A measured "
                    "one only means a block overran, which a single scheduling hiccup can "
                    "produce.\n\n"
                    "The core is where the audio thread was last SEEN, sampled one block in "
                    "64 -- where it has been, not where it is.");
    }

    ~LoadReadout() override { stopTimer(); }

    void setSource (std::function<lighthost::load::Readout()> source)
    {
        readoutSource = std::move (source);
        updateTimerState();
    }

    void visibilityChanged()        override { updateTimerState(); }
    void parentHierarchyChanged()   override { updateTimerState(); }
    void refreshTimerForVisibility() override { updateTimerState(); }

    void paint (juce::Graphics& g) override
    {
        g.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));

        if (shown.text.isEmpty())
        {
            g.setColour (findColour (juce::Label::textColourId).withAlpha (0.35f));
            g.drawText ("--", getLocalBounds(), juce::Justification::centredLeft);
            return;
        }

        g.setColour (colourFor (shown.severity));
        g.drawText (shown.text, getLocalBounds(), juce::Justification::centredLeft);
    }

private:
    [[nodiscard]] juce::Colour colourFor (lighthost::load::Severity severity) const
    {
        switch (severity)
        {
            case lighthost::load::Severity::hot:     return juce::Colour (lighthost::ui::LookAndFeel::kHot);
            case lighthost::load::Severity::caution: return juce::Colour (lighthost::ui::LookAndFeel::kCaution);
            case lighthost::load::Severity::ok:      break;
        }

        return findColour (juce::Label::textColourId).withAlpha (0.85f);
    }

    void updateTimerState()
    {
        if (isShowing() && readoutSource)
            startTimer (lighthost::load::kPollMs);
        else
            stopTimer();
    }

    void timerCallback() override
    {
        // Re-checked rather than trusted: the same reason SignalMeter does it.
        if (! isShowing() || ! readoutSource)
        {
            stopTimer();
            return;
        }

        const auto next = readoutSource();

        // Only when it actually moved. The figure behind this is an average,
        // so it changes slightly on most polls and identically on many.
        if (next.text != shown.text || next.severity != shown.severity)
        {
            shown = next;
            repaint();
        }
    }

    std::function<lighthost::load::Readout()> readoutSource;
    lighthost::load::Readout shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoadReadout)
};


class PreferencesContentComponent final : public juce::Component,
                                          public  prefs::HeightReportingPanel,
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
        std::function<lighthost::metering::Meter* (int)> probeMeterAt,
        std::function<std::vector<juce::String>()> committedChainNames,
        std::function<void (const juce::String&, const juce::String&)> onDevicesChosen,
        std::function<lighthost::load::Readout()> audioLoadReadout)
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

        // The ONLY evidence that a device name was chosen rather than settled
        // on. rebuildDeviceCombos populates and selects with
        // dontSendNotification, so nothing but a person picking from the list
        // can get here -- which is exactly the distinction
        // recordRequestedDevices depends on and cannot make for itself.
        //
        // Reading the combo's text at Apply is NOT that evidence, and assuming
        // it was is how this went wrong the first time: after a fallback the
        // combo displays the SUBSTITUTED device, because the missing one is
        // not in the list at all. Apply is also the button for chain edits,
        // sample rate and buffer size, so pressing it for any of those would
        // have recorded the substitute as the request and silenced the
        // substitution report for ever.
        inputDeviceCombo.onChange  = [this] { deviceChoiceIsUserMade = true; };

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
        // 9.0.2. See the standing checks in BACKLOG.md, and the loopback grep
        // tools/update-juce.sh runs on every bump.
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
        // Write site 1 of 3 for committedBaseline. The others are the end of
        // setChain and the apply lambda in commitAllSettings; see the member's
        // declaration for why there are exactly three.
        committedBaseline = lighthost::ui::chainRowsFrom (activeChain, bypassStates, laneStates);
        chainList.setRows (committedBaseline);
        chainList.onChange = [this] { updateChainListHeight(); chainList.repaint(); };
        chainList.onEditClicked = [this] (int i)
        {
            if (onEditPluginFn && i < static_cast<int> (chainList.getRows().size()))
                onEditPluginFn (chainList.getRows()[static_cast<size_t> (i)].description);
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

        // See the input combo above for why this flag is the only honest
        // signal available.
        outputDeviceCombo.onChange = [this] { deviceChoiceIsUserMade = true; };
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

        onSignalViewToggledFn  = std::move (onSignalViewToggled);
        probeMeterAtFn         = std::move (probeMeterAt);
        committedChainNamesFn  = std::move (committedChainNames);
        onDevicesChosenFn      = std::move (onDevicesChosen);

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

        addAndMakeVisible (loadHeadLabel);
        addAndMakeVisible (loadReadout);
        loadHeadLabel.setText ("Audio load:", juce::dontSendNotification);
        loadHeadLabel.setFont (juce::Font (juce::FontOptions{}.withHeight (13.0f)));
        loadHeadLabel.setJustificationType (juce::Justification::centredRight);
        loadReadout.setSource (std::move (audioLoadReadout));

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

    /** Folds the committed chain into the staged one.

        This used to assign the three vectors straight over the top, which
        discarded every unapplied edit -- and it is reached from nine sites,
        including a plugin merely re-declaring its latency. See the
        reconcileStagedChain banner in Source/PluginChainStore.hpp for the rule
        and for why the merge needs a baseline as well as the two chains.
    */
    void setChain (const std::vector<juce::PluginDescription>& chain,
                   const std::vector<bool>& bypass, const std::vector<int>& laneStates)
    {
        const auto incoming = lighthost::ui::chainRowsFrom (chain, bypass, laneStates);

        const auto plan = lighthost::chain::reconcileStagedChain (
            lighthost::ui::entriesFor (chainList.getRows()),
            lighthost::ui::entriesFor (committedBaseline),
            lighthost::ui::entriesFor (incoming));

        // The plan is indices, because a chain::ChainEntry holds an identity
        // hash and cannot rebuild a description. Materialised here, against the
        // same two vectors that produced it.
        ChainRows merged;
        merged.reserve (plan.rows.size());

        for (const auto& row : plan.rows)
            merged.push_back (row.source == lighthost::chain::MergedRow::Source::staged
                                  ? chainList.getRows()[row.index]
                                  : incoming[row.index]);

        chainList.setRows (std::move (merged));

        // Write site 2 of 3. ALWAYS, and always from the incoming chain rather
        // than from the merged result: the baseline records what has been
        // committed, and the merged list is by definition not that.
        committedBaseline = incoming;

        // Counted so the apply path can tell whether this ran while it was
        // inside the host. See write site 3.
        ++committedBaselineWrites;

        updateChainListHeight();
        chainList.repaint();

        // The chain just changed, so its declared latency probably did too.
        updateLatencyDisplay();

        // Only when the list moved under the user. A kept edit is silent, which
        // is the point: gating on "was there an edit at all" instead would put
        // a message on screen every time any of the nine refresh sites fired,
        // for as long as an edit was pending.
        if (plan.droppedStagedRows + plan.addedIncomingRows > 0)
            setApplyFeedback (chainChangedElsewhereMessage (plan));
    }

    /** What the panel says when a refresh moved rows the user had not applied.

        Beside the Apply button rather than in the status row, because the status
        row prefixes "Problem: " for something that is neither a problem nor
        unexpected, is shared with IconMenu's status sink so the next report
        erases it, and calls resized() -- which would jolt the whole layout every
        time a plugin re-declared its latency.

        "edits kept" is literally true whatever the counts are: a dropped row is
        always one that existed at the baseline, because reconcileStagedChain
        can only drop a row it found there, so no plugin the user added is ever
        among them.
    */
    [[nodiscard]] static juce::String chainChangedElsewhereMessage (
        const lighthost::chain::Reconciliation& plan)
    {
        juce::StringArray parts;

        if (plan.addedIncomingRows > 0)
            parts.add ("+" + juce::String (plan.addedIncomingRows));

        if (plan.droppedStagedRows > 0)
            parts.add ("-" + juce::String (plan.droppedStagedRows));

        return "Chain changed elsewhere (" + parts.joinIntoString (", ") + "); edits kept";
    }

    /** Rebuilds the signal view's rows from the current chain.

        Called when the view opens and whenever the chain changes, because the
        probes are recreated on both and the meters they belong to move with
        them.
    */
    void refreshSignalView (SignalViewPanel& panel, int chainLatencySamplesNow)
    {
        // Both rules live in SignalView.hpp, next to the component they decide
        // the contents of, and both are asserted in Tests/SignalViewTests.cpp.
        // They were written out here, inside a class no test can construct, so
        // the one that matters most -- that an EMPTY committed chain is not a
        // reason to fall back to the staged rows -- had never been checked.
        const auto rowNames = lighthost::ui::signalViewRowNames (committedChainNamesFn,
                                                                 chainList.getRows());

        const auto rate = [this]
        {
            auto* device = deviceManager.getCurrentAudioDevice();
            return device != nullptr ? device->getCurrentSampleRate() : 0.0;
        }();

        // The Output row's right-hand text: the chain's latency in milliseconds,
        // or nothing at all when no device is open and the figure has no unit.
        const auto outputDetail =
            rate > 0.0 ? juce::String (chainLatencySamplesNow * 1000.0 / rate, 1) + " ms"
                       : juce::String{};

        auto contents = lighthost::ui::signalViewContents (rowNames,
                                                           inputDeviceCombo.getText(),
                                                           deviceInputMeter,
                                                           outputDetail,
                                                           deviceOutputMeter,
                                                           probeMeterAtFn);

        panel.setTaps (std::move (contents.taps), contents.atProbeLimit);
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
        const bool wasShowing = statusLabel.isVisible();

        statusLabel.setVisible (show);
        showLogButton.setVisible (show);

        // Through the funnel only when the row actually appeared or vanished:
        // the fixed height moves on a flip, not on every status report.
        if (show != wasShowing)
            optionalRowsChanged();
        else
            resized();
    }

    /** Which optional rows are on screen right now. */
    [[nodiscard]] prefs::OptionalRows optionalRows() const
    {
        prefs::OptionalRows rows;
        rows.status = statusLabel.isVisible();

        #if JUCE_WINDOWS
        rows.virtualInputHint = virtualInputHint.isVisible();
        #endif

        return rows;
    }

    /** Height of everything except the chain viewport, which is the only section
        that stretches.

        The sum lives in PreferencesLayout.hpp, where it is asserted. It was
        written out here, agreeing with resized() below only by care -- and the
        comment beside it had already drifted by 34 pixels once.
    */
    [[nodiscard]] int fixedLayoutHeight() const
    {
        return prefs::fixedLayoutHeight (kMetrics, optionalRows());
    }

    /** The shortest this panel can be laid out at without a section losing its
        height. The scrolling viewport that holds it never sizes it below this, so
        a window too short for the layout scrolls instead of quietly eating the
        Apply button.
    */
    [[nodiscard]] int getPreferredHeight() const override
    {
        return prefs::preferredHeight (kMetrics, optionalRows());
    }

    /** Relays out, having first asked the owner to re-height this panel.

        EVERY OPTIONAL ROW GOES THROUGH HERE. resized() alone is not enough: it
        divides up the height the panel ALREADY has, and the chain viewport is
        the only elastic section -- so once that is at its floor there is
        nothing left to take a new row from, and the Apply button is laid out
        below the bottom edge with nothing re-running the viewport so it could
        be scrolled to.

        No re-entrancy guard, deliberately. resized() calls only
        updateChainListHeight(), and nothing in the layout path changes the
        visibility of either optional row. The one thing that does --
        rebuildDeviceCombos into updateVirtualInputHint -- is reached from
        changeListenerCallback, never from resized(). A second piece of state
        to keep correct, for a loop that cannot start, is a cost with no
        benefit.
    */
    void optionalRowsChanged()
    {
        const auto before = getLocalBounds();

        if (onPreferredHeightChanged != nullptr)
            onPreferredHeightChanged();

        // setSize only calls resized() when the bounds actually moved, and they
        // do not when the window is already tall enough. So relay out here in
        // exactly the case the owner did not.
        if (getLocalBounds() == before)
            resized();

        // For the smoke test, which is the only automated exercise of this
        // path: its seeded plugin fails to load, so a real status message
        // lands on a real window on all three platforms. Suppressed before the
        // first layout, where the panel has no size to report.
        if (getWidth() > 0)
            juce::Logger::writeToLog (
                juce::String ("Preferences: optional rows changed (status=")
                + (statusLabel.isVisible() ? "1" : "0") + "), panel "
                + juce::String (getHeight()) + " of " + juce::String (getPreferredHeight())
                + (getHeight() >= getPreferredHeight() ? " -- fits" : " -- DOES NOT FIT"));
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

        // Directly under Latency, because the two answer the same question
        // from opposite ends: how much delay the chain adds, and how close
        // it is to not fitting in the time it has. The readout takes the
        // rest of the row rather than a fixed width -- its text is much
        // longer than the latency figure and grows when dropouts appear.
        {
            constexpr int kLabelW = 90;
            auto row = area.removeFromTop (kRowH);
            loadHeadLabel.setBounds (row.removeFromLeft (kLabelW));
            loadReadout.setBounds   (row.reduced (4, 2));
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
    /** Set by either device combo's onChange, cleared once the choice has been
        recorded. False means every device name on screen was put there by
        rebuildDeviceCombos, which reflects what is OPEN -- after a fallback,
        the substitute.
    */
    bool deviceChoiceIsUserMade = false;

    juce::ComboBox inputDeviceCombo;
    juce::Label    inputChannelLabel;

    #if JUCE_WINDOWS
    juce::Label      virtualInputHint;
    juce::TextButton getCableButton;
    #endif

    // AUDIO CHAIN
    SectionLabel            chainSectionLabel  { "  AUDIO CHAIN" };
    AudioChainListComponent chainList;

    /** The committed chain the staged list was built from.

        The third input reconcileStagedChain needs. Without it a row that is in
        the staged list and not in the incoming one is ambiguous: it is either a
        plugin the user added and has not applied, or one deleted from the tray
        while the window was open, and the two want opposite answers.

        EXACTLY THREE WRITE SITES, all marked in this file: the constructor, the
        end of setChain, and the apply lambda in commitAllSettings. A fourth
        would have to justify itself -- and a missing third is what would make a
        deleted plugin reappear, because the applied chain would go on being
        compared against a baseline from before the Apply.

        Site 3 is CONDITIONAL, which the first version of this comment did not
        say. onApplyFn returns void, so it cannot report that the host abandoned
        the edit -- and on that path the host rolls its settings back and
        refreshes this panel, which reaches setChain and sets the baseline from
        the chain that is actually committed. Writing stagedRows over it
        afterwards recorded a chain that was never committed, after which
        staged == baseline and the next refresh took the no-op fast path and
        replaced the rows silently. So site 3 defers to site 2 when site 2 ran
        during the apply.
    */
    ChainRows committedBaseline;

    /** How many times setChain has written the baseline.

        Only ever compared against itself across a call into the host, to answer
        "did a refresh happen in there?". Wraps harmlessly.
    */
    unsigned int committedBaselineWrites = 0;

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
    // One struct, so the height report and resized() cannot hold different
    // numbers. The aliases below keep resized() reading as it always did.
    static constexpr prefs::Metrics kMetrics {};

    static constexpr int kPad       = kMetrics.pad;
    static constexpr int kSectH     = kMetrics.sectionH;
    static constexpr int kRowH      = kMetrics.rowH;
    static constexpr int kGap       = kMetrics.gap;
    static constexpr int kBtnH      = kMetrics.buttonH;
    static constexpr int kMinChainH = kMetrics.minChainH;

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
    juce::Label    loadHeadLabel;
    LoadReadout    loadReadout;
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
    std::function<std::vector<juce::String>()> committedChainNamesFn;
    std::function<void (const juce::String&, const juce::String&)> onDevicesChosenFn;

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

            // The hint occupies a layout row only while visible, so this takes
            // the same re-height path the status row does.
            optionalRowsChanged();
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
        // Snapshotted with the rest, for the same reason: reading it live
        // inside the lambda would see whatever the combos did after the click.
        const bool deviceChosen = deviceChoiceIsUserMade;
        const int  rateId   = sampleRateCombo.getSelectedId();
        const int  bufId    = bufferSizeCombo.getSelectedId();
        // The chain, the bypass flags and the lane assignments are one value and
        // are snapshotted as one. Reading any of them live inside the lambda
        // below would apply an edit made after the click against a chain from
        // before it: lanes used to be read that way, so a lane change between
        // the click and the dispatch landed on the wrong plugin. They are now
        // literally one value, which is what stops that being re-introduced.
        const auto stagedRows = chainList.getRows();

        // callAsync lets the button visually grey out before we block the message
        // thread with device restarts.
        juce::Component::SafePointer<PreferencesContentComponent> safe (this);
        juce::MessageManager::callAsync (
            [safe, typeName, inName, outName, deviceChosen, rateId, bufId, stagedRows]
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

                    // A device is written only when the user picked one, or
                    // when the combo already agrees with what is open.
                    //
                    // Without the first condition, Apply opens a device nobody
                    // asked for. rebuildDeviceCombos starts at selection 1 and
                    // only moves off it on a name match, so when no input
                    // device is open the combo displays the FIRST one in the
                    // list -- and that name is in availableInputs, so the old
                    // guard passed it straight through. An output-only user
                    // who opened Preferences to change the buffer size got a
                    // microphone opened for them.
                    const auto acceptable = [] (const juce::String& name,
                                                const juce::StringArray& available)
                    {
                        return name.isNotEmpty()
                            && ! name.startsWith ("(")     // a placeholder, not a device
                            && available.contains (name);
                    };

                    if (acceptable (inName, availableInputs)
                        && (deviceChosen || inName == setup.inputDeviceName))
                        setup.inputDeviceName = inName;

                    if (acceptable (outName, availableOutputs)
                        && (deviceChosen || outName == setup.outputDeviceName))
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

                // ONLY when a person picked from a device list. Apply is also
                // the button for chain edits, sample rate and buffer size, and
                // the combo text is not evidence of a choice: after a fallback
                // it shows the SUBSTITUTED device, because the one that went
                // missing is not in the list at all.
                //
                // Recording unconditionally, as the first version of this did,
                // inverts the whole feature. Startup reports the substitution,
                // the user opens Preferences to look at it, presses Apply for
                // any reason, and the substitute is written down as the thing
                // they asked for -- after which nothing can ever notice it
                // again. deviceChoiceIsUserMade is set only by the combos'
                // onChange, and rebuildDeviceCombos populates them with
                // dontSendNotification, so it cannot be set by us.
                //
                // setup.inputDeviceName rather than the raw combo text,
                // because the guards above drop a name the device type does
                // not offer and placeholders like "(no input devices)".
                if (deviceChosen && safe != nullptr && safe->onDevicesChosenFn)
                {
                    safe->onDevicesChosenFn (setup.inputDeviceName, setup.outputDeviceName);

                    // Cleared only after the record is written, so a failed
                    // Apply does not quietly forget that a choice was made.
                    safe->deviceChoiceIsUserMade = false;
                }

                // 3. Plugin chain + bypass states
                if (safe != nullptr && safe->onApplyFn)
                {
                    // Converted here rather than held as three vectors: the
                    // host's apply path is the boundary the trio survives at,
                    // and inside the panel there is only ever one row list.
                    const auto applied = lighthost::ui::chainVectorsFrom (stagedRows);

                    const auto writesBefore = safe->committedBaselineWrites;

                    safe->onApplyFn (applied.chain, applied.bypassed, applied.lanes);

                    // Write site 3 of 3, and the load-bearing one. The staged
                    // chain has just BECOME the committed chain, so it is the
                    // baseline from here. Without this, applying {A, B, C} would
                    // leave the baseline at whatever it was before -- say {A} --
                    // and the next tray delete of B would find B in neither the
                    // incoming chain nor the baseline, read it as a pending
                    // addition, and put the plugin the user just deleted back.
                    //
                    // Skipped when setChain ran inside onApplyFn, because then
                    // the host has already set the baseline from the chain it
                    // actually committed, and that is better information than
                    // stagedRows.
                    //
                    // This is what the abandon path does: onApplyFn is void, so
                    // it cannot report that the edit was rolled back, and on
                    // that path the host rewires and refreshes this panel. The
                    // refresh set the baseline correctly; writing stagedRows
                    // over it recorded a chain that was never committed, leaving
                    // staged == baseline so the next refresh took the no-op fast
                    // path and swapped the rows with nothing said. On the
                    // success path a refresh sets the same thing stagedRows
                    // would, so deferring costs nothing there.
                    //
                    // Re-checked for null because onApplyFn reaches IconMenu,
                    // which can close this window.
                    if (safe != nullptr && safe->committedBaselineWrites == writesBefore)
                        safe->committedBaseline = stagedRows;
                }

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

                // One hop through the message loop before the second chooser
                // opens. Opening it REPLACES chainTestChooser, which destroys
                // `fc` from inside its own completion callback. That survives
                // today only because juce::FileChooser::finished copies the
                // callback out before invoking it and touches nothing
                // afterwards -- an internal detail of a vendored class, not a
                // contract, and a JUCE bump could change it.
                //
                // The hop does not guard that detail, it removes the dependence
                // on it: by the time the second chooser is built, this callback
                // has returned and `fc` is no longer on the stack. It costs one
                // message-loop turn. DO NOT SIMPLIFY THIS BACK.
                juce::MessageManager::callAsync ([safe, input]
                {
                    if (safe != nullptr)
                        safe->chooseChainTestOutput (input);
                });
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
        // Through contentSizeFor, which subtracts the scrollbar only when a bar
        // will actually be shown. This used to subtract it unconditionally, and
        // it is the one place this file's two viewport patterns disagreed. It
        // also used to bail out entirely while the viewport had no width,
        // leaving the list at its previous HEIGHT -- the half that matters.
        const auto size = prefs::contentSizeFor (chainViewport,
                                                 chainList.getPreferredHeight());
        chainList.setSize (size.x, size.y);
    }

    void showAddPluginMenu()
    {
        // Which plugins are already staged, so their menu entries can be greyed.
        //
        // Uses the settings store's definition of "the same plugin" rather than a
        // second one written here. This was
        //
        //     fileOrIdentifier + pluginFormatName + name
        //
        // spelled out twice in this function, and it disagreed with
        // Store::identityOf in both directions. It included `name`, so a plugin renamed by an update
        // read as a different plugin and could be added a second time even though
        // the store would then treat both rows as one. And it omitted the unique
        // ids, which are the only thing telling apart the several plugins a shell
        // plugin packs into one file, so two of those sharing a display name
        // greyed each other out.
        std::set<juce::String> active;
        for (const auto& row : chainList.getRows())
            active.insert (lighthost::chain::Store::identityOf (row.description));

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
            subMenu.addItem (id++, pd.name,
                             active.count (lighthost::chain::Store::identityOf (pd)) == 0);
        }

        if (groupStarted)
            menu.addSubMenu (labelFor (currentMfr), subMenu);

        if (types.isEmpty())
            menu.addItem (1, "(no plugins scanned)", false);

        juce::Component::SafePointer<PreferencesContentComponent> safe (this);
        menu.showMenuAsync (
            juce::PopupMenu::Options{}.withTargetComponent (addPluginButton),
            // "captured" rather than reusing the name: an init-capture that
            // shadows the local it moves from compiles, and then the next
            // person to add a line inside the lambda cannot tell which one
            // they are touching.
            [safe, captured = std::move (types)] (int result) mutable
            {
                if (safe == nullptr) return;
                // juce::Array counts and indexes with int, not size_t, so the
                // casts that used to be here converted an int to size_t and
                // straight back again -- narrowing on the return trip, which is
                // what Clang's -Wshorten-64-to-32 was pointing at.
                if (result <= 0 || result > captured.size()) return;

                // One row rather than a push into each of three vectors and a
                // resize to put them back in step. The trio it used to keep in
                // lockstep by hand no longer exists.
                //
                // addRow now de-duplicates and fires onChange itself, so the
                // updateChainListHeight/repaint pair that used to sit here --
                // an open copy of onChange's body -- has gone with it. It
                // refuses a plugin the greying above thought was unreachable:
                // the greying is a snapshot, and the chain can change while the
                // menu is open.
                const auto& chosen = captured[result - 1];

                if (! safe->chainList.addRow ({ chosen, false, 0 }))
                    safe->setApplyFeedback (chosen.name + " is already in the chain");
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
//
// The class itself is now prefs::PanelViewport in Source/PreferencesLayout.hpp,
// so the re-height hook is installed by the viewport's own constructor rather
// than by whoever builds it. That wiring is the thing that was missing: an
// optional row appearing grew the height the panel needed and nothing told the
// viewport, so the panel was laid out inside its old height and the Apply
// button went below the bottom edge -- unreachable, because the viewport did
// not know it had more to scroll either.
//==============================================================================
using PreferencesPanelViewport = prefs::PanelViewport<PreferencesContentComponent>;

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
    std::function<std::vector<juce::String>()> committedChainNames,
    std::function<void (const juce::String& inputName,
                        const juce::String& outputName)> onDevicesChosen,
    std::function<lighthost::load::Readout()> audioLoadReadout,
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
        [this, notifyHost = std::move (onSignalViewToggled)] (bool enabled)
        {
            if (notifyHost)
                notifyHost (enabled);

            setSignalViewOpen (enabled);
        },
        std::move (probeMeterAt),
        std::move (committedChainNames),
        std::move (onDevicesChosen),
        std::move (audioLoadReadout));

    // Height budget. The authority is fixedLayoutHeight(), not this comment --
    // which had drifted 34px out of date within one release of being written,
    // when the Latency row landed and was added to the arithmetic but not to the
    // prose. What follows is a sanity check on that function, not a second
    // source of truth.
    //
    // Above the chain: 10 pad + 62 INPUT + 26 input meter + 34 virtual-input hint
    // when shown + 28 chain label = 160. Below it: 40 Add Plugin + 62 LANE TRIM +
    // 62 OUTPUT + 26 output meter + 198 DEVICE SETTINGS, being its label plus
    // five rows -- API, rate, buffer, Latency, Audio load -- + 50 buttons and
    // pad = 438. With the chain viewport at its 80px minimum that is 678 of
    // content with the hint showing, 644 without it.
    //
    // The default height below is 650, which is the WINDOW including its native
    // title bar -- so the shell gets roughly 619, under what the layout wants,
    // and the viewport scrolls. That is what it is for, and why adding a row no
    // longer means re-deriving a window size. Stated explicitly because comparing
    // 650 against 644 suggests the opposite conclusion, and the arithmetic in
    // this comment has already drifted once.
    //
    // It is safe to leave alone now in a way it was not before: the panel is
    // re-heighted when an optional row appears, so the Apply button stays
    // reachable by scrolling instead of being laid out past the bottom edge.
    // See Source/PreferencesLayout.hpp.
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

    // Leaving fullscreen FIRST, for two reasons: a programmatic resize fights a
    // window the OS has maximised, and the width remembered below has to be the
    // restored width. Reading it while still maximised meant closing the view
    // later resized the window to the width of the screen.
    if (isFullScreen())
        setFullScreen (false);

    if (shouldBeOpen)
    {
        // Remembered, not derived. Subtracting the panel width on the way back
        // would lose any resizing the user did while it was open.
        widthBeforeSignalView = getWidth();

        if (auto* panel = shell->panel())
            panel->refreshSignalView (shell->getSignalView(), latencySamples());
    }

    shell->setSignalViewVisible (shouldBeOpen);

    // The floor comes from the constrainer setResizeLimits installed, rather
    // than a second copy of the number.
    const auto minWidth = getConstrainer() != nullptr ? getConstrainer()->getMinimumWidth()
                                                      : getWidth();

    const auto target = shouldBeOpen
                            ? getWidth() + SignalViewPanel::kWidth
                            : juce::jmax (minWidth, widthBeforeSignalView);

    setSize (target, getHeight());
}

void PreferencesWindow::minimisationStateChanged (bool isNowMinimised)
{
    DocumentWindow::minimisationStateChanged (isNowMinimised);

    // The whole point: this is the only component JUCE tells. See the
    // declaration in PreferencesWindow.h for why nothing below it finds out.
    if (auto* content = getContentComponent())
        lighthost::ui::refreshVisibilityTimers (*content);
}
