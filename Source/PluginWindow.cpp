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
        // Gives the message queue one turn before the caller destroys the
        // processors these editors belonged to.
        //
        // What it is for: a plugin's editor is its own UI toolkit, not ours.
        // Closing one can leave posted messages or COM releases outstanding that
        // want delivering before the AudioProcessor underneath goes away. This
        // host loads plugins in-process, so that clean-up runs here or not at
        // all. Current JUCE's equivalent (PluginGraph::closeAnyOpenPluginWindows)
        // does not pump, and neither does closeCurrentlyOpenWindowsFor below --
        // which is suggestive, but three no-pump call sites closing one node's
        // windows is weaker evidence than it looks. Removing this needs a soak
        // across real VST2 and browser-hosting editors, not an argument.
        //
        // What cannot run during it: the modal state is the point. It installs
        // isEventBlockedByModalComps, so user input to other windows is swallowed
        // while internal messages, timers and window destruction still flow. The
        // caller is expected to have quiesced its own rewiring first --
        // IconMenu::loadActivePlugins cancels its AsyncUpdater and detaches its
        // processor listeners immediately before calling this, so a plugin
        // reporting a latency change on the way out cannot rebuild a graph that
        // is about to be cleared.
        //
        // The graph itself is still whole here. This runs between editor
        // teardown and graph teardown, not during it.
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
            ui = new GenericAudioProcessorEditor (*processor);   // JUCE 8: takes reference
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
