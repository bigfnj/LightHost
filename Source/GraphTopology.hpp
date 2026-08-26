#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

//==============================================================================
// Deciding how the plugin chain is wired, as a pure function.
//
// Facts about the nodes go in, the complete set of connections the graph should
// hold comes out. Nothing here touches the graph, the settings file or the audio
// device, so every routing rule below is testable offline with no plugin and no
// device (see Tests/GraphTopologyTests.cpp).
//
// Why this is not done inline against the live graph any more:
//
// Versions up to 4.0.3 wired the chain by trying a connection and reacting to the
// return value, six times over:
//
//     addConnection (src ch1 -> dst ch1);
//     if (! addConnection (src ch2 -> dst ch2))
//         addConnection (src ch1 -> dst ch2);
//
// That reads as "duplicate mono to stereo", and it worked only because the
// caller had just removed every connection in the graph, so the sole reason
// addConnection could fail was an out-of-range channel. The moment connections
// are diffed rather than torn down, the same false return also means "that
// connection is already there", and the fallback silently wires the left channel
// into the right. The check has to be made against known channel counts instead
// of a return value, and that means knowing the counts up front, which is what
// Layout carries.
//
// There is deliberately no delay compensation here. juce::AudioProcessorGraph
// accumulates each node's getLatencySamples() along every path and pads the
// shorter ones itself, bypassed or not (juce_AudioProcessorGraph.cpp:1460), so a
// second layer of padding would double-compensate. That is exactly the bug fixed
// in 5.0.0; see the note in IconMenu::reconnectGraph.
//==============================================================================
namespace lighthost::topology
{
    using NodeID     = juce::AudioProcessorGraph::NodeID;
    using Connection = juce::AudioProcessorGraph::Connection;

    /** Highest lane index the Preferences UI offers, so lanes are 0-3. */
    static constexpr int kMaxLane = 3;

    /** Light Host is a stereo host: it wires at most two channels per edge. */
    static constexpr int kMaxChannels = 2;

    /** One plugin node's wiring-relevant facts, read off the live graph.

        Channel counts are the node's negotiated bus layout, not what it declared
        when it was scanned, so a plugin that came up mono is wired as mono.
    */
    struct NodeFacts
    {
        NodeID nodeId;
        int    lane              = 0;
        int    numInputChannels  = 0;
        int    numOutputChannels = 0;
    };

    /** Everything needed to decide the wiring: the graph's two IO nodes, how many
        channels each of them carries, and the chain in order.
    */
    struct Layout
    {
        NodeID inputNodeId;
        NodeID outputNodeId;

        /** Channels available from the graph's input node. One, on a mono input
            device, which is where the mono-to-stereo duplication comes from.
        */
        int inputNodeChannels = 0;

        /** Channels the graph's output node accepts. */
        int outputNodeChannels = 0;

        /** The chain in order. Each node's lane decides which lane it lands in;
            order within a lane is the order of this vector.
        */
        std::vector<NodeFacts> nodes;
    };

    /** A node can only sit in a serial lane if audio can get in and back out of
        it. A node with no input or no output channels (an instrument, a metering
        plugin, or a plugin whose bus negotiation failed) would otherwise break
        the lane it was placed in, so the caller wires around it.
    */
    [[nodiscard]] inline bool canPassAudio (const NodeFacts& node) noexcept
    {
        return node.numInputChannels > 0 && node.numOutputChannels > 0;
    }

    /** Appends the connections for one edge of the chain.

        Destination channel 0 is fed from source channel 0. Destination channel 1,
        if it has one, is fed from source channel 1 when the source has one and
        from source channel 0 when it does not, which is the mono-to-stereo
        duplication a mono input device relies on. A mono destination takes
        channel 0 only: its right channel does not exist, and inventing a
        connection to it is what the old return-value fallback did.
    */
    inline void appendEdge (std::vector<Connection>& connections,
                            NodeID sourceId,
                            int sourceChannels,
                            NodeID destinationId,
                            int destinationChannels)
    {
        if (sourceChannels <= 0 || destinationChannels <= 0)
            return;

        const auto channels = std::min (kMaxChannels, destinationChannels);

        for (int destinationChannel = 0; destinationChannel < channels; ++destinationChannel)
        {
            const auto sourceChannel = std::min (destinationChannel, sourceChannels - 1);
            connections.push_back ({ { sourceId, sourceChannel },
                                     { destinationId, destinationChannel } });
        }
    }

    /** Returns every connection the graph should hold for this layout.

        Nodes are grouped into lanes, each lane is chained in order from the input
        node to the output node, and the lanes therefore sum at the output node.
        With no usable node in any lane the input is wired straight to the output,
        so audio keeps flowing when every plugin is missing or unusable.

        The result is deterministic: the same layout always produces the same
        connections in the same order, which is what lets the caller diff it
        against the live graph and apply only the difference.
    */
    [[nodiscard]] inline std::vector<Connection> buildConnections (const Layout& layout)
    {
        std::vector<Connection> connections;

        std::map<int, std::vector<const NodeFacts*>> lanes;

        for (const auto& node : layout.nodes)
            if (canPassAudio (node))
                lanes[juce::jlimit (0, kMaxLane, node.lane)].push_back (&node);

        if (lanes.empty())
        {
            appendEdge (connections,
                        layout.inputNodeId, layout.inputNodeChannels,
                        layout.outputNodeId, layout.outputNodeChannels);
            return connections;
        }

        for (const auto& lane : lanes)
        {
            auto previousId       = layout.inputNodeId;
            auto previousChannels = layout.inputNodeChannels;

            for (const auto* node : lane.second)
            {
                appendEdge (connections,
                            previousId, previousChannels,
                            node->nodeId, node->numInputChannels);

                previousId       = node->nodeId;
                previousChannels = node->numOutputChannels;
            }

            appendEdge (connections,
                        previousId, previousChannels,
                        layout.outputNodeId, layout.outputNodeChannels);
        }

        return connections;
    }

    /** What applyConnections had to change, for logging and for tests. */
    struct DiffResult
    {
        int added   = 0;
        int removed = 0;

        /** Connections the graph refused. Always empty in practice: the layout
            only names channels the nodes have. A non-empty list means a lane has
            gone silent, so the caller logs it rather than dropping it.
        */
        std::vector<Connection> refused;
    };

    /** Makes the graph's connections match `desired`, touching only what differs.

        Every mutation passes UpdateKind::none, so the caller must call
        graph.rebuild() afterwards. That is the whole point: one render sequence
        published to the audio thread instead of one per connection, and no
        intermediate sequence in which the chain is half wired.
    */
    inline DiffResult applyConnections (juce::AudioProcessorGraph& graph,
                                        const std::vector<Connection>& desired)
    {
        const std::set<Connection> wanted (desired.begin(), desired.end());

        const auto existingConnections = graph.getConnections();
        const std::set<Connection> existing (existingConnections.begin(),
                                             existingConnections.end());

        DiffResult result;

        for (const auto& connection : existing)
            if (wanted.find (connection) == wanted.end())
                if (graph.removeConnection (connection, juce::AudioProcessorGraph::UpdateKind::none))
                    ++result.removed;

        for (const auto& connection : wanted)
        {
            if (existing.find (connection) != existing.end())
                continue;

            if (graph.addConnection (connection, juce::AudioProcessorGraph::UpdateKind::none))
                ++result.added;
            else
                result.refused.push_back (connection);
        }

        return result;
    }
}
