#include "PluginWindow.h"
#include <exception>

using namespace juce;

//==============================================================================
Array<PluginWindow*>& PluginWindow::getActiveWindows()
{
    static Array<PluginWindow*> windows;
    return windows;
}

//==============================================================================
PluginWindow::PluginWindow (Component* const pluginEditor,
                            AudioProcessorGraph::Node::Ptr o,
                            WindowFormatType t)
    : DocumentWindow (pluginEditor->getName(),
                      LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                      DocumentWindow::minimiseButton | DocumentWindow::closeButton),
      owner (std::move (o)),
      type (t)
{
    setSize (400, 300);
    setUsingNativeTitleBar (true);
    setContentOwned (pluginEditor, true);

    setTopLeftPosition (owner->properties.getWithDefault (getLastXProp (type), Random::getSystemRandom().nextInt (500)),
                        owner->properties.getWithDefault (getLastYProp (type), Random::getSystemRandom().nextInt (500)));

    owner->properties.set (getOpenProp (type), true);
    setVisible (true);

    getActiveWindows().add (this);
}

PluginWindow::~PluginWindow()
{
    getActiveWindows().removeFirstMatchingValue (this);
    clearContentComponent();
}

//==============================================================================
void PluginWindow::closeCurrentlyOpenWindowsFor (AudioProcessorGraph::NodeID nodeId)
{
    auto& windows = getActiveWindows();
    for (int i = windows.size(); --i >= 0;)
        if (windows.getUnchecked (i)->owner->nodeID == nodeId)
            delete windows.getUnchecked (i);
}

void PluginWindow::closeAllCurrentlyOpenWindows()
{
    auto& windows = getActiveWindows();
    if (windows.size() > 0)
    {
        for (int i = windows.size(); --i >= 0;)
            delete windows.getUnchecked (i);

        #if JUCE_MODAL_LOOPS_PERMITTED
        Component dummyModalComp;
        dummyModalComp.enterModalState();
        MessageManager::getInstance()->runDispatchLoopUntil (50);
        #endif
    }
}

bool PluginWindow::containsActiveWindows()
{
    return getActiveWindows().size() > 0;
}

//==============================================================================
// A concrete PropertyComponent for displaying program names
class ProgramPropertyComponent final : public PropertyComponent
{
public:
    explicit ProgramPropertyComponent (const String& name)
        : PropertyComponent (name) {}

    void refresh() override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgramPropertyComponent)
};

// A simple editor that displays the program list as properties
class ProgramAudioProcessorEditor final : public AudioProcessorEditor
{
public:
    explicit ProgramAudioProcessorEditor (AudioProcessor* const p)
        : AudioProcessorEditor (p)
    {
        jassert (p != nullptr);
        setOpaque (true);
        addAndMakeVisible (panel);

        Array<PropertyComponent*> programs;
        const auto numPrograms = p->getNumPrograms();
        int totalHeight = 0;

        for (int i = 0; i < numPrograms; ++i)
        {
            auto name = p->getProgramName (i).trim();
            if (name.isEmpty())
                name = "Unnamed";

            auto* pc = new ProgramPropertyComponent (name);
            programs.add (pc);
            totalHeight += pc->getPreferredHeight();
        }

        panel.addProperties (programs);
        setSize (400, jlimit (25, 400, totalHeight));
    }

    void paint (Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (ResizableWindow::backgroundColourId));
    }
    void resized() override           { panel.setBounds (getLocalBounds()); }

private:
    PropertyPanel panel;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgramAudioProcessorEditor)
};

//==============================================================================
PluginWindow* PluginWindow::getWindowFor (AudioProcessorGraph::Node::Ptr node,
                                          WindowFormatType type)
{
    jassert (node != nullptr);

    auto& windows = getActiveWindows();
    for (int i = windows.size(); --i >= 0;)
    {
        auto* w = windows.getUnchecked (i);
        // Normal falls back to Generic when no native editor exists, so a
        // Normal request should also match an existing Generic window for the
        // same node — otherwise calling Edit twice on a plugin with no native
        // UI creates two duplicate Generic windows.
        if (w->owner == node && (w->type == type || (type == Normal && w->type == Generic)))
            return w;
    }

    auto* processor = node->getProcessor();
    if (processor == nullptr)
        return nullptr;

    AudioProcessorEditor* ui = nullptr;

    try
    {
        if (type == Normal)
        {
            // JUCE 8.0.13+ replaced createEditorIfNeeded() with createEditorAndMakeActive(),
            // which also registers the editor pointer back on the AudioProcessor so
            // getActiveEditor() returns the correct result.
            ui = processor->createEditorAndMakeActive();
            if (ui == nullptr)
                type = Generic;
        }

        if (ui == nullptr)
        {
            if (type == Generic || type == Parameters)
                ui = new GenericAudioProcessorEditor (*processor);   // JUCE 8: takes reference
            else if (type == Programs)
                ui = new ProgramAudioProcessorEditor (processor);
        }
    }
    catch (const std::exception& e)
    {
        Logger::writeToLog ("PluginWindow: editor creation threw std::exception: " + String (e.what()));
        return nullptr;
    }
    catch (...)
    {
        Logger::writeToLog ("PluginWindow: editor creation threw unknown exception");
        return nullptr;
    }

    if (ui != nullptr)
    {
        if (auto* const plugin = dynamic_cast<AudioPluginInstance*> (processor))
            ui->setName (plugin->getName());

        return new PluginWindow (ui, std::move (node), type);
    }

    return nullptr;
}

//==============================================================================
void PluginWindow::moved()
{
    owner->properties.set (getLastXProp (type), getX());
    owner->properties.set (getLastYProp (type), getY());
}

void PluginWindow::closeButtonPressed()
{
    owner->properties.set (getOpenProp (type), false);
    delete this;
}
