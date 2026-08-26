#include "../Source/GainProcessor.hpp"
#include "../Source/GraphTopology.hpp"
#include "StubProcessors.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <set>

//==============================================================================
// Wiring rules for the plugin chain, tested as a pure function.
//
// These cover the cases the old return-value-driven wiring could not express:
// mono sources and mono destinations told apart from "already connected", nodes
// that cannot carry audio at all, and lane indices that came out of a settings
// file unvalidated. The last two tests are the load-bearing ones for
// IconMenu::reconnectGraph: every connection must name a channel that actually
// exists (or the graph will refuse it and a lane goes silent), and the same
// layout must always produce the same connections (or diffing against the live
// graph would churn on every call).
//==============================================================================
namespace
{
    using namespace lighthost::topology;

    constexpr NodeID kInput  { 1'000'000u };
    constexpr NodeID kOutput { 1'000'001u };

    NodeFacts plugin (juce::uint32 id, int lane = 0, int numIns = 2, int numOuts = 2)
    {
        return { NodeID { id }, lane, numIns, numOuts };
    }

    Layout layoutOf (std::vector<NodeFacts> nodes,
                     int inputChannels = 2,
                     int outputChannels = 2)
    {
        Layout layout;
        layout.inputNodeId       = kInput;
        layout.outputNodeId      = kOutput;
        layout.inputNodeChannels = inputChannels;
        layout.outputNodeChannels = outputChannels;
        layout.nodes             = std::move (nodes);
        return layout;
    }

    /** Reserved ids for the lane trims, matching IconMenu's. */
    constexpr NodeID laneGain (int lane) { return NodeID { 1'000'010u + (juce::uint32) lane }; }

    /** Gives every lane a trim node. */
    Layout withLaneGains (Layout layout)
    {
        for (int lane = 0; lane <= kMaxLane; ++lane)
            layout.laneGainNodeIds[(size_t) lane] = laneGain (lane);

        return layout;
    }

    bool has (const std::vector<Connection>& connections,
              NodeID sourceId, int sourceChannel,
              NodeID destinationId, int destinationChannel)
    {
        const Connection wanted { { sourceId, sourceChannel },
                                  { destinationId, destinationChannel } };
        return std::find (connections.begin(), connections.end(), wanted) != connections.end();
    }

    bool touches (const std::vector<Connection>& connections, NodeID nodeId)
    {
        return std::any_of (connections.begin(), connections.end(),
                            [nodeId] (const Connection& c)
                            {
                                return c.source.nodeID == nodeId
                                    || c.destination.nodeID == nodeId;
                            });
    }

    int countFrom (const std::vector<Connection>& connections, NodeID nodeId)
    {
        return (int) std::count_if (connections.begin(), connections.end(),
                                    [nodeId] (const Connection& c)
                                    { return c.source.nodeID == nodeId; });
    }

    int countInto (const std::vector<Connection>& connections, NodeID nodeId)
    {
        return (int) std::count_if (connections.begin(), connections.end(),
                                    [nodeId] (const Connection& c)
                                    { return c.destination.nodeID == nodeId; });
    }

    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;

    /** A real graph with real IO nodes, and the Layout that describes it.

        The channel counts come off the processors exactly as
        IconMenu::reconnectGraph reads them, so a wiring rule that names a channel
        a node does not have is refused here in the same way it would be refused
        in the running app.
    */
    struct LiveGraph
    {
        LiveGraph()
        {
            // Channel counts must be set before the IO nodes are added:
            // AudioGraphIOProcessor sizes its buses from the parent graph.
            graph.setBusesLayout ({ { juce::AudioChannelSet::stereo() },
                                    { juce::AudioChannelSet::stereo() } });

            auto inputNode  = graph.addNode (std::make_unique<IOProc> (IOProc::audioInputNode));
            auto outputNode = graph.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode));

            layout.inputNodeId        = inputNode->nodeID;
            layout.outputNodeId       = outputNode->nodeID;
            layout.inputNodeChannels  = inputNode->getProcessor()->getTotalNumOutputChannels();
            layout.outputNodeChannels = outputNode->getProcessor()->getTotalNumInputChannels();
        }

        NodeID add (int lane, int numIns = 2, int numOuts = 2)
        {
            auto node = graph.addNode (std::make_unique<lighthost::test::ChannelStub> (numIns, numOuts));
            auto* processor = node->getProcessor();

            layout.nodes.push_back ({ node->nodeID,
                                      lane,
                                      processor->getTotalNumInputChannels(),
                                      processor->getTotalNumOutputChannels() });

            return node->nodeID;
        }

        /** Adds a real trim node per lane at the reserved ids, as IconMenu does. */
        void addLaneGains()
        {
            for (int lane = 0; lane <= kMaxLane; ++lane)
            {
                auto node = graph.addNode (std::make_unique<lighthost::gain::Processor>(),
                                           laneGain (lane));

                if (node != nullptr)
                    layout.laneGainNodeIds[(size_t) lane] = node->nodeID;
            }
        }

        DiffResult apply()
        {
            const auto result = applyConnections (graph, buildConnections (layout));
            graph.rebuild();
            return result;
        }

        std::set<Connection> live() const
        {
            const auto current = graph.getConnections();
            return { current.begin(), current.end() };
        }

        juce::AudioProcessorGraph graph;
        Layout layout;
    };
}

//==============================================================================
class GraphTopologyTests final : public juce::UnitTest
{
public:
    GraphTopologyTests()
        : juce::UnitTest ("Chain wiring", "GraphTopology") {}

    void runTest() override
    {
        beginTest ("an empty chain wires the input straight to the output");
        {
            const auto connections = buildConnections (layoutOf ({}));

            expectEquals ((int) connections.size(), 2);
            expect (has (connections, kInput, 0, kOutput, 0));
            expect (has (connections, kInput, 1, kOutput, 1));
        }

        beginTest ("one stereo plugin sits between the input and the output");
        {
            const auto p = plugin (10);
            const auto connections = buildConnections (layoutOf ({ p }));

            expectEquals ((int) connections.size(), 4);
            expect (has (connections, kInput, 0, p.nodeId, 0));
            expect (has (connections, kInput, 1, p.nodeId, 1));
            expect (has (connections, p.nodeId, 0, kOutput, 0));
            expect (has (connections, p.nodeId, 1, kOutput, 1));
        }

        beginTest ("plugins on the same lane are chained in list order");
        {
            const auto first  = plugin (10);
            const auto second = plugin (11);
            const auto connections = buildConnections (layoutOf ({ first, second }));

            expect (has (connections, kInput, 0, first.nodeId, 0));
            expect (has (connections, first.nodeId, 0, second.nodeId, 0));
            expect (has (connections, second.nodeId, 0, kOutput, 0));

            // The second plugin is fed by the first, not by the input, and the
            // first does not reach the output directly.
            expect (! has (connections, kInput, 0, second.nodeId, 0));
            expect (! has (connections, first.nodeId, 0, kOutput, 0));
        }

        beginTest ("lanes run in parallel and sum at the output");
        {
            const auto laneZero = plugin (10, 0);
            const auto laneOne  = plugin (11, 1);
            const auto connections = buildConnections (layoutOf ({ laneZero, laneOne }));

            expect (has (connections, kInput, 0, laneZero.nodeId, 0));
            expect (has (connections, kInput, 0, laneOne.nodeId, 0));
            expect (has (connections, laneZero.nodeId, 0, kOutput, 0));
            expect (has (connections, laneOne.nodeId, 0, kOutput, 0));

            // Parallel means parallel: nothing crosses between the lanes.
            expect (! has (connections, laneZero.nodeId, 0, laneOne.nodeId, 0));
            expect (! has (connections, laneOne.nodeId, 0, laneZero.nodeId, 0));
            expectEquals (countInto (connections, kOutput), 4);
        }

        beginTest ("a mono input feeds both channels of the first plugin");
        {
            // A mono capture device gives the graph's input node one channel.
            // Duplicating it is the 3.5.0 behaviour, and it must survive the
            // move away from try-and-see-if-it-failed wiring.
            const auto p = plugin (10);
            const auto connections = buildConnections (layoutOf ({ p }, /*inputChannels*/ 1));

            expect (has (connections, kInput, 0, p.nodeId, 0));
            expect (has (connections, kInput, 0, p.nodeId, 1));
            expect (! has (connections, kInput, 1, p.nodeId, 1), "the input has no channel 1");
        }

        beginTest ("a mono output plugin feeds both channels of the next plugin");
        {
            const auto monoOut = plugin (10, 0, 2, /*numOuts*/ 1);
            const auto next    = plugin (11);
            const auto connections = buildConnections (layoutOf ({ monoOut, next }));

            expect (has (connections, monoOut.nodeId, 0, next.nodeId, 0));
            expect (has (connections, monoOut.nodeId, 0, next.nodeId, 1));
            expect (! has (connections, monoOut.nodeId, 1, next.nodeId, 1));
        }

        beginTest ("a mono input plugin takes channel 0 and nothing is invented");
        {
            // The old code tried source ch1 -> dest ch1, and on failure tried
            // source ch0 -> dest ch1, which is a connection to a channel the
            // plugin does not have. Both calls failed, so the behaviour was
            // right by accident; here it is right by construction.
            const auto monoIn = plugin (10, 0, /*numIns*/ 1, 2);
            const auto connections = buildConnections (layoutOf ({ monoIn }));

            expect (has (connections, kInput, 0, monoIn.nodeId, 0));
            expectEquals (countInto (connections, monoIn.nodeId), 1);
        }

        beginTest ("a mono output node feeding a mono graph output stays mono");
        {
            const auto p = plugin (10, 0, 2, 1);
            const auto connections = buildConnections (layoutOf ({ p }, 2, /*outputChannels*/ 1));

            expect (has (connections, p.nodeId, 0, kOutput, 0));
            expectEquals (countInto (connections, kOutput), 1);
        }

        beginTest ("a node that cannot pass audio is wired around, not through");
        {
            // A plugin with no output channels (an analyser, or a failed bus
            // negotiation) would break the lane it sits in: everything after it
            // would be fed from a node that produces nothing.
            const auto before = plugin (10);
            const auto broken = plugin (11, 0, 2, /*numOuts*/ 0);
            const auto after  = plugin (12);
            const auto connections = buildConnections (layoutOf ({ before, broken, after }));

            expect (! touches (connections, broken.nodeId));
            expect (has (connections, before.nodeId, 0, after.nodeId, 0));
            expect (has (connections, after.nodeId, 0, kOutput, 0));
        }

        beginTest ("a chain of only unusable nodes still passes audio through");
        {
            const auto connections = buildConnections (
                layoutOf ({ plugin (10, 0, 0, 0), plugin (11, 1, 2, 0) }));

            expectEquals ((int) connections.size(), 2);
            expect (has (connections, kInput, 0, kOutput, 0));
            expect (has (connections, kInput, 1, kOutput, 1));
        }

        beginTest ("an out-of-range lane is clamped, not turned into a new lane");
        {
            // Nothing validates the lane value in the settings file. A corrupt or
            // hand-edited one must not create a fifth lane.
            const auto sane    = plugin (10, kMaxLane);
            const auto corrupt = plugin (11, 99);
            const auto negative = plugin (12, -5);
            const auto connections = buildConnections (layoutOf ({ sane, corrupt, negative }));

            // lane 99 clamps onto lane 3 and chains behind the plugin already
            // there; lane -5 clamps onto lane 0 and starts its own lane.
            expect (has (connections, sane.nodeId, 0, corrupt.nodeId, 0));
            expect (has (connections, kInput, 0, negative.nodeId, 0));
            expectEquals (countFrom (connections, kInput), 4, "two lanes, two channels each");
        }

        beginTest ("every connection names a channel that exists");
        {
            // This is what lets reconnectGraph apply connections with
            // UpdateKind::none and trust them: an out-of-range channel would be
            // refused by the graph, and the lane would go silent with only a log
            // line to show for it.
            const auto layout = layoutOf ({ plugin (10, 0, 1, 1),
                                            plugin (11, 0, 2, 2),
                                            plugin (12, 1, 2, 1),
                                            plugin (13, 2, 1, 2) },
                                          1, 2);
            const auto connections = buildConnections (layout);

            const auto channelsOut = [&layout] (NodeID id)
            {
                if (id == layout.inputNodeId)  return layout.inputNodeChannels;
                if (id == layout.outputNodeId) return 0;

                for (const auto& node : layout.nodes)
                    if (node.nodeId == id)
                        return node.numOutputChannels;

                return -1;
            };

            const auto channelsIn = [&layout] (NodeID id)
            {
                if (id == layout.outputNodeId) return layout.outputNodeChannels;
                if (id == layout.inputNodeId)  return 0;

                for (const auto& node : layout.nodes)
                    if (node.nodeId == id)
                        return node.numInputChannels;

                return -1;
            };

            for (const auto& c : connections)
            {
                expect (c.source.channelIndex >= 0
                            && c.source.channelIndex < channelsOut (c.source.nodeID),
                        "source channel out of range");
                expect (c.destination.channelIndex >= 0
                            && c.destination.channelIndex < channelsIn (c.destination.nodeID),
                        "destination channel out of range");
            }
        }

        beginTest ("a lane's trim sits between its last plugin and the output");
        {
            const auto first  = plugin (10);
            const auto second = plugin (11);
            const auto connections = buildConnections (withLaneGains (layoutOf ({ first, second })));

            expect (has (connections, second.nodeId, 0, laneGain (0), 0));
            expect (has (connections, second.nodeId, 1, laneGain (0), 1));
            expect (has (connections, laneGain (0), 0, kOutput, 0));
            expect (has (connections, laneGain (0), 1, kOutput, 1));

            // Nothing bypasses the trim on its way out.
            expect (! has (connections, second.nodeId, 0, kOutput, 0),
                    "the last plugin still reaches the output directly");
            expectEquals (countInto (connections, kOutput), 2);
        }

        beginTest ("each lane passes through its own trim, and only its own");
        {
            const auto laneZero = plugin (10, 0);
            const auto laneTwo  = plugin (11, 2);
            const auto connections = buildConnections (withLaneGains (layoutOf ({ laneZero, laneTwo })));

            expect (has (connections, laneZero.nodeId, 0, laneGain (0), 0));
            expect (has (connections, laneTwo.nodeId,  0, laneGain (2), 0));

            expect (! has (connections, laneZero.nodeId, 0, laneGain (2), 0));
            expect (! has (connections, laneTwo.nodeId,  0, laneGain (0), 0));

            // Two lanes, two trims, two channels each.
            expectEquals (countInto (connections, kOutput), 4);

            // The trims of the two empty lanes are not wired to anything.
            expect (! touches (connections, laneGain (1)));
            expect (! touches (connections, laneGain (3)));
        }

        beginTest ("a mono lane is spread to stereo by its trim");
        {
            // The trim is stereo, so a lane ending in a mono plugin arrives at the
            // output on both channels rather than only the left.
            const auto monoOut = plugin (10, 0, 2, 1);
            const auto connections = buildConnections (withLaneGains (layoutOf ({ monoOut })));

            expect (has (connections, monoOut.nodeId, 0, laneGain (0), 0));
            expect (has (connections, monoOut.nodeId, 0, laneGain (0), 1));
            expect (has (connections, laneGain (0), 1, kOutput, 1));
        }

        beginTest ("a lane with no trim node is wired straight to the output");
        {
            // Robustness: if a trim node could not be added to the graph, audio
            // must still get out.
            const auto only = plugin (10);
            const auto connections = buildConnections (layoutOf ({ only }));

            expect (has (connections, only.nodeId, 0, kOutput, 0));
            expect (! touches (connections, laneGain (0)));
        }

        beginTest ("an empty chain does not route through a trim");
        {
            const auto connections = buildConnections (withLaneGains (layoutOf ({})));

            expectEquals ((int) connections.size(), 2);
            expect (has (connections, kInput, 0, kOutput, 0));
            expect (! touches (connections, laneGain (0)));
        }

        beginTest ("the same layout always produces the same connections, with no duplicates");
        {
            // reconnectGraph diffs this result against the live graph. If the
            // function were unstable, or emitted the same connection twice, the
            // diff would add and remove connections on every call and rebuild the
            // render sequence for no reason.
            const auto layout = layoutOf ({ plugin (10, 0), plugin (11, 0),
                                            plugin (12, 2), plugin (13, 3, 2, 1) });

            const auto first  = buildConnections (layout);
            const auto second = buildConnections (layout);

            expect (first == second, "the same layout produced different wiring");

            const std::set<Connection> unique (first.begin(), first.end());
            expectEquals ((int) unique.size(), (int) first.size(), "a connection was emitted twice");
        }
    }
};

static GraphTopologyTests graphTopologyTests;

//==============================================================================
// The other half: applying that wiring to a graph that can refuse it.
//
// These are the regression tests for the click-on-every-edit bug. Up to 4.0.3
// rewiring meant removing every connection and adding them all back, one
// UpdateKind::sync mutation at a time, so the audio thread saw partly wired
// sequences on every bypass toggle. The guarantee now is that applying a wiring
// twice is a no-op, and that a small change makes a small number of mutations.
//==============================================================================
class ChainRewiringTests final : public juce::UnitTest
{
public:
    ChainRewiringTests()
        : juce::UnitTest ("Chain rewiring against a live graph", "GraphTopology") {}

    void runTest() override
    {
        beginTest ("the IO nodes report the graph's own channel count");
        {
            const LiveGraph g;

            expectEquals (g.layout.inputNodeChannels, 2);
            expectEquals (g.layout.outputNodeChannels, 2);
        }

        beginTest ("applying a wiring to a fresh graph installs exactly it");
        {
            LiveGraph g;
            g.add (0);
            g.add (0);

            const auto desired = buildConnections (g.layout);
            const auto diff    = g.apply();

            expect (diff.refused.empty(), "the graph refused a connection we computed");
            expectEquals (diff.added, (int) desired.size());
            expectEquals (diff.removed, 0);

            const std::set<Connection> wanted (desired.begin(), desired.end());
            expect (g.live() == wanted, "the graph does not hold the wiring we asked for");
        }

        beginTest ("applying the same wiring again changes nothing");
        {
            // The anti-churn guarantee. If this fails, every reconnectGraph call
            // tears down and reinstates connections, which is what used to make
            // the audio thread see a half-wired chain.
            LiveGraph g;
            g.add (0);
            g.add (1);
            g.apply();

            const auto again = g.apply();

            expectEquals (again.added, 0);
            expectEquals (again.removed, 0);
            expect (again.refused.empty());
        }

        beginTest ("moving one plugin to another lane rewires only its own edges");
        {
            LiveGraph g;
            g.add (0);
            g.add (0);
            g.apply();

            // Same as dragging the second plugin's Lane dropdown from 0 to 1.
            g.layout.nodes[1].lane = 1;
            const auto diff = g.apply();

            // Gone: the serial link between them. New: the second plugin's own
            // feed from the input, and the first plugin's own path to the output.
            expectEquals (diff.removed, 2, "more than the serial link was torn down");
            expectEquals (diff.added, 4);
            expect (diff.refused.empty());
        }

        beginTest ("a mono link in the middle of a chain is accepted by the graph");
        {
            // Stereo in, mono out, feeding mono in, stereo out. This is the case
            // the old try-and-fall-back wiring could not express once connections
            // were diffed rather than rebuilt.
            LiveGraph g;
            const auto monoOut = g.add (0, 2, 1);
            const auto monoIn  = g.add (0, 1, 2);

            const auto diff = g.apply();
            const auto live = g.live();

            expect (diff.refused.empty(), "the graph refused a mono connection");
            expect (live.count ({ { monoOut, 0 }, { monoIn, 0 } }) == 1);
            expect (live.count ({ { monoOut, 0 }, { monoIn, 1 } }) == 0,
                    "wired into a channel the mono plugin does not have");
            expect (live.count ({ { monoIn, 0 }, { g.layout.outputNodeId, 0 } }) == 1);
            expect (live.count ({ { monoIn, 1 }, { g.layout.outputNodeId, 1 } }) == 1);
        }

        beginTest ("a node that cannot carry audio is left unconnected");
        {
            LiveGraph g;
            const auto working = g.add (0);
            const auto broken  = g.add (0, 2, /*numOuts*/ 0);

            const auto diff = g.apply();
            const auto live = g.live();

            expect (diff.refused.empty());

            const bool touchesBroken =
                std::any_of (live.begin(), live.end(), [broken] (const Connection& c)
                             {
                                 return c.source.nodeID == broken
                                     || c.destination.nodeID == broken;
                             });

            expect (! touchesBroken, "wired a node that cannot pass audio");
            expect (live.count ({ { working, 0 }, { g.layout.outputNodeId, 0 } }) == 1,
                    "the rest of the lane should still reach the output");
        }

        beginTest ("the graph accepts a chain wired through real trim nodes");
        {
            LiveGraph g;
            g.addLaneGains();
            const auto onLaneZero = g.add (0);
            const auto onLaneOne  = g.add (1);

            const auto diff = g.apply();
            const auto live = g.live();

            expect (diff.refused.empty(), "the graph refused a connection to a trim node");
            expect (live.count ({ { onLaneZero, 0 }, { laneGain (0), 0 } }) == 1);
            expect (live.count ({ { onLaneOne,  0 }, { laneGain (1), 0 } }) == 1);
            expect (live.count ({ { laneGain (0), 0 }, { g.layout.outputNodeId, 0 } }) == 1);
            expect (live.count ({ { laneGain (1), 0 }, { g.layout.outputNodeId, 0 } }) == 1);
        }

        beginTest ("adding a trim to an existing chain rewires only the summing point");
        {
            // The trims are created once at load. This is the other order: a chain
            // wired without them, then rewired with them, which is what a diff has
            // to handle without tearing the lane down.
            LiveGraph g;
            const auto only = g.add (0);
            g.apply();

            g.addLaneGains();
            const auto diff = g.apply();

            expect (diff.refused.empty());
            expectEquals (diff.removed, 2, "more than the final edge was torn down");
            expectEquals (diff.added, 4, "plugin to trim, and trim to output");
            expect (g.live().count ({ { only, 0 }, { laneGain (0), 0 } }) == 1);
        }

        beginTest ("removing every plugin falls back to a direct connection");
        {
            LiveGraph g;
            g.add (0);
            g.apply();

            g.layout.nodes.clear();
            const auto diff = g.apply();
            const auto live = g.live();

            expect (diff.refused.empty());
            expectEquals ((int) live.size(), 2);
            expect (live.count ({ { g.layout.inputNodeId, 0 }, { g.layout.outputNodeId, 0 } }) == 1);
            expect (live.count ({ { g.layout.inputNodeId, 1 }, { g.layout.outputNodeId, 1 } }) == 1);
        }
    }
};

static ChainRewiringTests chainRewiringTests;
