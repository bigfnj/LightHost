#include "../Source/PluginWindow.h"
#include "StubProcessors.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// Plugin editor windows.
//
// This file had no tests, and it holds three fixes that each cost a debugging
// session: a plugin with no native editor opening two duplicate Generic windows,
// a plugin whose editor constructor throws taking the host down with it, and a
// deprecated JUCE editor-creation call that left getActiveEditor() wrong.
//
// It is split into two categories because they need different things:
//
//   "PluginWindow"     no window is ever constructed, so it runs anywhere,
//                      including a CI runner with no display. This is where the
//                      throwing-editor fix lives -- getWindowFor returns before
//                      it builds anything.
//
//   "PluginWindowGui"  constructs real DocumentWindows, so it needs a display
//                      server. Registered as a separate CTest entry behind the
//                      same xvfb check the smoke tests already use, rather than
//                      being folded into the main run where it would segfault
//                      on headless Linux.
//
// One trap worth naming: the teardown path in closeAllCurrentlyOpenWindows is
// wrapped in JUCE_MODAL_LOOPS_PERMITTED, which defaults to 0. Without that
// define on the test target, a test of that function would exercise a DIFFERENT
// function from the one that ships. CMakeLists.txt sets it to 1 for this reason.
//==============================================================================
namespace
{
    using Graph = juce::AudioProcessorGraph;

    /** Has a native editor, and throws when asked to build it.

        createEditorAndMakeActive is non-virtual, so the throw goes in
        createEditor, which it calls unguarded. hasEditor() returns true so the
        Normal path actually asks.
    */
    class ThrowingEditorStub final : public juce::AudioProcessor
    {
    public:
        ThrowingEditorStub()
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override        { return "ThrowingEditorStub"; }
        bool acceptsMidi() const override                  { return false; }
        bool producesMidi() const override                 { return false; }
        double getTailLengthSeconds() const override       { return 0.0; }
        int getNumPrograms() override                       { return 1; }
        int getCurrentProgram() override                    { return 0; }
        void setCurrentProgram (int) override               {}
        const juce::String getProgramName (int) override    { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override  {}
        void prepareToPlay (double, int) override            {}
        void releaseResources() override                     {}

        bool hasEditor() const override { return true; }

        juce::AudioProcessorEditor* createEditor() override
        {
            throw std::runtime_error ("this plugin's editor cannot be built");
        }

        using juce::AudioProcessor::processBlock;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThrowingEditorStub)
    };

    /** Throws something that is not a std::exception, to reach the catch-all. */
    class OddThrowStub final : public juce::AudioProcessor
    {
    public:
        OddThrowStub()
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override        { return "OddThrowStub"; }
        bool acceptsMidi() const override                  { return false; }
        bool producesMidi() const override                 { return false; }
        double getTailLengthSeconds() const override       { return 0.0; }
        int getNumPrograms() override                       { return 1; }
        int getCurrentProgram() override                    { return 0; }
        void setCurrentProgram (int) override               {}
        const juce::String getProgramName (int) override    { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override  {}
        void prepareToPlay (double, int) override            {}
        void releaseResources() override                     {}

        bool hasEditor() const override { return true; }

        juce::AudioProcessorEditor* createEditor() override { throw 42; }

        using juce::AudioProcessor::processBlock;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OddThrowStub)
    };
}

//==============================================================================
class PluginWindowTests final : public juce::UnitTest
{
public:
    PluginWindowTests()
        : juce::UnitTest ("Plugin editor windows", "PluginWindow") {}

    void runTest() override
    {
        beginTest ("the window registry starts and stays empty when nothing opens");
        {
            expect (! PluginWindow::containsActiveWindows(),
                    "another test left a window open, which would make the "
                    "assertions below meaningless");
        }

        beginTest ("an editor constructor that throws does not escape");
        {
            // A third-party plugin throwing out of its own editor constructor
            // used to take the host process with it. There is nothing this
            // application can do about the throw except refuse to open a window.
            Graph graph;
            auto node = graph.addNode (std::make_unique<ThrowingEditorStub>(),
                                       Graph::NodeID { 1 });
            expect (node != nullptr);

            PluginWindow* window = nullptr;

            try
            {
                window = PluginWindow::getWindowFor (node, PluginWindow::Normal);
            }
            catch (...)
            {
                expect (false, "getWindowFor let the plugin's exception escape");
            }

            expect (window == nullptr, "a failed editor must not yield a window");
            expect (! PluginWindow::containsActiveWindows(),
                    "a half-built window was left in the registry");
        }

        beginTest ("a throw that is not a std::exception is caught too");
        {
            // The catch-all exists because plugin code is not obliged to throw
            // anything sensible.
            //
            // Normal, not Generic: a Generic request skips
            // createEditorAndMakeActive altogether and constructs the generic
            // editor directly, so createEditor is never called and there is
            // nothing to throw. Only the Normal path asks the plugin.
            Graph graph;
            auto node = graph.addNode (std::make_unique<OddThrowStub>(), Graph::NodeID { 1 });

            PluginWindow* window = nullptr;

            try
            {
                window = PluginWindow::getWindowFor (node, PluginWindow::Normal);
            }
            catch (...)
            {
                expect (false, "the catch-all did not catch it");
            }

            expect (window == nullptr);
            expect (! PluginWindow::containsActiveWindows());
        }

        beginTest ("closing all windows when none are open is harmless");
        {
            // Called on every chain reload, usually with nothing open.
            PluginWindow::closeAllCurrentlyOpenWindows();
            expect (! PluginWindow::containsActiveWindows());
        }

        beginTest ("closing windows for a node with none open is harmless");
        {
            PluginWindow::closeCurrentlyOpenWindowsFor (Graph::NodeID { 1 });
            PluginWindow::closeCurrentlyOpenWindowsFor (Graph::NodeID { 999 });
            expect (! PluginWindow::containsActiveWindows());
        }

        //======================================================================
        beginTest ("the settings keys are derived from the window type");
        {
            // The window-position keys are built from the type name, so a
            // renamed or added enumerator silently orphans stored positions.
            expectEquals (toString (PluginWindow::Normal),  juce::String ("Normal"));
            expectEquals (toString (PluginWindow::Generic), juce::String ("Generic"));

            expectEquals (getLastXProp (PluginWindow::Normal),  juce::String ("uiLastX_Normal"));
            expectEquals (getLastYProp (PluginWindow::Generic), juce::String ("uiLastY_Generic"));

            expect (getLastXProp (PluginWindow::Normal) != getLastXProp (PluginWindow::Generic),
                    "the two window types must not share a stored position");
        }
    }
};

static PluginWindowTests pluginWindowTests;

//==============================================================================
/** The half that needs a display server. See the note at the top of this file. */
class PluginWindowGuiTests final : public juce::UnitTest
{
public:
    PluginWindowGuiTests()
        : juce::UnitTest ("Plugin editor windows (GUI)", "PluginWindowGui") {}

    void runTest() override
    {
        beginTest ("asking twice for a plugin with no native editor reuses one window");
        {
            // The fix this covers: Normal falls back to Generic when a plugin has
            // no editor of its own, and the lookup used to match only on the
            // requested type. So a second Edit produced a second
            // GenericAudioProcessorEditor on the same AudioProcessor -- two
            // windows, and two parameter listeners registered.
            Graph graph;
            auto node = graph.addNode (std::make_unique<lighthost::test::PassthroughStub>(),
                                       Graph::NodeID { 1 });

            auto* first = PluginWindow::getWindowFor (node, PluginWindow::Normal);
            expect (first != nullptr, "no window was created at all");
            expect (PluginWindow::containsActiveWindows());

            auto* second = PluginWindow::getWindowFor (node, PluginWindow::Normal);
            expect (second == first,
                    "a second Normal request created a duplicate Generic window");

            // And asking for Generic explicitly finds the same one.
            expect (PluginWindow::getWindowFor (node, PluginWindow::Generic) == first);

            PluginWindow::closeAllCurrentlyOpenWindows();
            expect (! PluginWindow::containsActiveWindows(),
                    "the window outlived closeAllCurrentlyOpenWindows");
        }

        beginTest ("two different plugins get two different windows");
        {
            Graph graph;
            auto a = graph.addNode (std::make_unique<lighthost::test::PassthroughStub>(),
                                    Graph::NodeID { 1 });
            auto b = graph.addNode (std::make_unique<lighthost::test::PassthroughStub>(),
                                    Graph::NodeID { 2 });

            auto* windowA = PluginWindow::getWindowFor (a, PluginWindow::Normal);
            auto* windowB = PluginWindow::getWindowFor (b, PluginWindow::Normal);

            expect (windowA != nullptr && windowB != nullptr);
            expect (windowA != windowB, "two plugins shared one editor window");

            PluginWindow::closeAllCurrentlyOpenWindows();
            expect (! PluginWindow::containsActiveWindows());
        }

        beginTest ("closing one plugin's windows leaves the others alone");
        {
            Graph graph;
            auto a = graph.addNode (std::make_unique<lighthost::test::PassthroughStub>(),
                                    Graph::NodeID { 1 });
            auto b = graph.addNode (std::make_unique<lighthost::test::PassthroughStub>(),
                                    Graph::NodeID { 2 });

            auto* windowA = PluginWindow::getWindowFor (a, PluginWindow::Normal);
            (void) PluginWindow::getWindowFor (b, PluginWindow::Normal);

            // This is the path taken when one plugin is removed from the chain.
            PluginWindow::closeCurrentlyOpenWindowsFor (Graph::NodeID { 2 });

            expect (PluginWindow::containsActiveWindows(),
                    "removing one plugin closed the other plugin's editor");
            expect (PluginWindow::getWindowFor (a, PluginWindow::Normal) == windowA,
                    "the surviving window should still be found, not rebuilt");

            PluginWindow::closeAllCurrentlyOpenWindows();
            expect (! PluginWindow::containsActiveWindows());
        }

        beginTest ("a moved window records its position against the type used");
        {
            // Read back by the constructor, so an editor reopens where it was.
            // Recorded against Generic, not Normal: the stub has no native
            // editor, so getWindowFor falls back and the window's type is
            // Generic -- storing under the requested type would put the position
            // in a key the next window does not read.
            Graph graph;
            auto node = graph.addNode (std::make_unique<lighthost::test::PassthroughStub>(),
                                       Graph::NodeID { 1 });

            auto* window = PluginWindow::getWindowFor (node, PluginWindow::Normal);
            expect (window != nullptr);

            window->setTopLeftPosition (123, 456);

            expectEquals ((int) node->properties.getWithDefault (
                              getLastXProp (PluginWindow::Generic), -1), 123);
            expectEquals ((int) node->properties.getWithDefault (
                              getLastYProp (PluginWindow::Generic), -1), 456);

            PluginWindow::closeAllCurrentlyOpenWindows();
        }
    }
};

static PluginWindowGuiTests pluginWindowGuiTests;
