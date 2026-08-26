#include "StubProcessors.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

//==============================================================================
// Offline AudioProcessorGraph render tests. No audio device, no real plugin, no
// GUI. These render actual audio through an actual graph and assert on samples.
//
// WHY THESE EXIST
//
// Light Host relies on juce::AudioProcessorGraph to keep parallel lanes
// sample-aligned. The graph does this by accumulating each node's
// getLatencySamples() along every path and inserting a delay op on the shorter
// ones. That behaviour is not part of JUCE's documented public contract — it
// lives inside the pimpl'd render-sequence builder — so it is pinned here.
//
// If a JUCE upgrade changes it, these tests fail and the lanes feature needs
// rethinking. That is exactly the signal we want, and it is the signal the
// previous test suite could not give: it verified the lane arithmetic in
// isolation while the assembled system was misaligned by 512 samples.
//==============================================================================
namespace
{
    using namespace lighthost::test;
    using Graph = juce::AudioProcessorGraph;
    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;

    constexpr int    kBlockSize  = 128;
    constexpr double kSampleRate = 48000.0;

    std::optional<int> findImpulse (const std::vector<float>& signal, float threshold)
    {
        for (size_t i = 0; i < signal.size(); ++i)
            if (signal[i] > threshold)
                return static_cast<int> (i);

        return std::nullopt;
    }

    /** Sends one unit impulse through a prepared graph and returns channel 0. */
    std::vector<float> render (Graph& graph, int numBlocks)
    {
        juce::AudioBuffer<float> audio (2, kBlockSize);
        juce::MidiBuffer midi;

        std::vector<float> out;
        out.reserve (static_cast<size_t> (kBlockSize) * static_cast<size_t> (numBlocks));

        for (int block = 0; block < numBlocks; ++block)
        {
            audio.clear();

            if (block == 0)
            {
                audio.setSample (0, 0, 1.0f);
                audio.setSample (1, 0, 1.0f);
            }

            graph.processBlock (audio, midi);

            for (int i = 0; i < kBlockSize; ++i)
                out.push_back (audio.getSample (0, i));
        }

        return out;
    }

    juce::String describeHits (const std::vector<float>& signal)
    {
        juce::String hits;

        for (size_t i = 0; i < signal.size(); ++i)
            if (std::abs (signal[i]) > 0.5f)
                hits << " [" << juce::String ((int) i) << "]=" << juce::String (signal[i], 2);

        return hits.isEmpty() ? juce::String (" none") : hits;
    }

    /** Stereo graph with IO nodes already added. Channel counts must be set
        before any connection is made: AudioGraphIOProcessor sizes its buses from
        the parent graph, so a graph reporting zero channels yields an input node
        with zero outputs and every addConnection is silently rejected.
    */
    struct StereoGraph
    {
        StereoGraph()
        {
            graph.setBusesLayout ({ { juce::AudioChannelSet::stereo() },
                                    { juce::AudioChannelSet::stereo() } });

            input  = graph.addNode (std::make_unique<IOProc> (IOProc::audioInputNode))->nodeID;
            output = graph.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode))->nodeID;
        }

        /** Adds a chain of latency-reporting stubs as one lane, input to output. */
        void addLane (const std::vector<int>& latencies, bool bypassed = false)
        {
            auto previous = input;

            for (const int latency : latencies)
            {
                auto node = graph.addNode (std::make_unique<LatencyStub> (latency));
                node->setBypassed (bypassed);

                connect (previous, node->nodeID);
                previous = node->nodeID;
            }

            connect (previous, output);
        }

        /** Adds one lane of a single LatencyStub and hands it back, so a test can
            change its latency after the graph has been prepared.
        */
        LatencyStub& addLaneReturningStub (int latency)
        {
            auto processor = std::make_unique<LatencyStub> (latency);
            auto& reference = *processor;

            auto node = graph.addNode (std::move (processor));
            connect (input, node->nodeID);
            connect (node->nodeID, output);

            return reference;
        }

        /** One lane holding a single plugin that owns its bypass parameter. */
        void addLaneWithBypassParameter (int latency, bool bypassed)
        {
            auto node = graph.addNode (std::make_unique<BypassParameterStub> (latency));
            node->setBypassed (bypassed);

            connect (input, node->nodeID);
            connect (node->nodeID, output);
        }

        void connect (Graph::NodeID from, Graph::NodeID to)
        {
            for (int ch = 0; ch < 2; ++ch)
                graph.addConnection ({ { from, ch }, { to, ch } });
        }

        void prepare()
        {
            graph.rebuild();
            graph.prepareToPlay (kSampleRate, kBlockSize);
        }

        Graph graph;
        Graph::NodeID input, output;
    };
}

//==============================================================================
class LaneAlignmentThroughGraphTests final : public juce::UnitTest
{
public:
    LaneAlignmentThroughGraphTests()
        : juce::UnitTest ("Parallel lane alignment through the graph", "GraphRender") {}

    void runTest() override
    {
        beginTest ("an empty graph passes audio straight through");
        {
            StereoGraph g;
            g.connect (g.input, g.output);
            g.prepare();

            const auto signal = render (g.graph, 4);
            const auto peak   = findImpulse (signal, 0.5f);

            expect (peak.has_value(), "no audio reached the output");

            if (peak.has_value())
                expectEquals (*peak, 0, "passthrough should not delay");
        }

        beginTest ("a single lane is delayed by exactly its own latency");
        {
            StereoGraph g;
            g.addLane ({ 512 });
            g.prepare();

            const auto peak = findImpulse (render (g.graph, 8), 0.5f);

            expect (peak.has_value(), "no audio reached the output");

            if (peak.has_value())
                expectEquals (*peak, 512);

            expectEquals (g.graph.getLatencySamples(), 512,
                          "the host should report the chain latency");
        }

        beginTest ("two lanes of differing latency arrive aligned");
        {
            expectLanesAligned ({ { 512 }, { 0 } }, 512);
        }

        beginTest ("four lanes of chained plugins arrive aligned");
        {
            // Lane totals: 0, 64, 329, 640. The 329 case crosses block boundaries,
            // which is where an off-by-one in the padding would surface.
            expectLanesAligned ({ { 0, 0 }, { 64, 0 }, { 129, 200 }, { 512, 128 } }, 640);
        }

        beginTest ("lanes of equal latency need no padding and stay aligned");
        {
            expectLanesAligned ({ { 256 }, { 256 }, { 256 } }, 256);
        }

        beginTest ("a bypassed lane stays aligned with the rest");
        {
            // A plugin that reports latency must produce that same latency when
            // bypassed, or the host's compensation shifts its audio forward in
            // time. JUCE asserts on plugins that break this, and LatencyStub
            // honours it, so bypassing must not disturb alignment.
            StereoGraph g;
            g.addLane ({ 256 });
            g.addLane ({ 256 }, /*bypassed*/ true);
            g.prepare();

            const auto signal = render (g.graph, 8);
            logMessage ("bypassed lane, non-zero samples:" + describeHits (signal));

            const auto summed = findImpulse (signal, 1.5f);
            expect (summed.has_value(), "bypassing a lane broke alignment");

            if (summed.has_value())
                expectEquals (*summed, 256);
        }

        beginTest ("a lane bypassed through the plugin's own parameter stays aligned");
        {
            // The other bypass branch. With a bypass parameter present the graph
            // does not bypass the node: the plugin keeps processing and keeps its
            // latency, so the compensation must keep applying. Treating this as a
            // zero-latency lane would pull it out of time with the other one and
            // cancel rather than sum, which is why the assertion below is on the
            // summed amplitude.
            StereoGraph g;
            g.addLaneWithBypassParameter (256, /*bypassed*/ false);
            g.addLaneWithBypassParameter (256, /*bypassed*/ true);
            g.prepare();

            const auto signal = render (g.graph, 8);
            logMessage ("bypass-parameter lane, non-zero samples:" + describeHits (signal));

            const auto summed = findImpulse (signal, 1.5f);
            expect (summed.has_value(),
                    "a lane bypassed through its own parameter fell out of alignment");

            if (summed.has_value())
                expectEquals (*summed, 256);

            expectEquals (g.graph.getLatencySamples(), 256,
                          "a plugin bypassed through its own parameter keeps its latency");
        }

        beginTest ("a latency change mid-session realigns the lanes once rebuilt");
        {
            // The graph records each node's latency when it builds its render
            // sequence and never revisits it on its own: it does not listen to
            // its nodes. So when a plugin switches to a linear-phase or
            // oversampled mode inside its own editor, every other lane stays
            // compensated by the old amount until something rebuilds.
            //
            // IconMenu listens for that change and rewires. This pins the half of
            // the mechanism that belongs to JUCE: that a rebuild is enough, and
            // the new latency is what gets compensated.
            StereoGraph g;
            auto& changing = g.addLaneReturningStub (0);
            g.addLane ({ 256 });
            g.prepare();

            const auto before = findImpulse (render (g.graph, 8), 1.5f);
            expect (before.has_value(), "the lanes did not start out aligned");

            if (before.has_value())
                expectEquals (*before, 256);

            // The plugin now claims more latency than the other lane.
            changing.changeLatencyTo (512);
            g.graph.rebuild();
            g.graph.prepareToPlay (kSampleRate, kBlockSize);

            const auto signal = render (g.graph, 16);
            logMessage ("after latency change, non-zero samples:" + describeHits (signal));

            const auto after = findImpulse (signal, 1.5f);
            expect (after.has_value(),
                    "the lanes did not realign after a plugin changed its latency");

            if (after.has_value())
                expectEquals (*after, 512, "compensation used the old latency");

            expectEquals (g.graph.getLatencySamples(), 512,
                          "the host should report the new slowest lane");
        }
    }

private:
    /** Builds one lane per entry, renders an impulse, and asserts every lane
        arrives on the same sample: a single impulse whose amplitude equals the
        lane count, landing at the slowest lane's latency.
    */
    void expectLanesAligned (const std::vector<std::vector<int>>& lanes, int expectedLatency)
    {
        StereoGraph g;

        for (const auto& lane : lanes)
            g.addLane (lane);

        g.prepare();

        const auto signal = render (g.graph, 24);
        logMessage (juce::String ((int) lanes.size()) + " lanes, non-zero samples:"
                    + describeHits (signal));

        // Amplitude equal to the lane count is the alignment assertion: if any
        // lane arrived at a different sample, no single sample can reach it.
        const auto threshold = static_cast<float> (lanes.size()) - 0.5f;
        const auto summed    = findImpulse (signal, threshold);

        expect (summed.has_value(),
                "lanes did not sum into one aligned impulse of amplitude "
                    + juce::String ((int) lanes.size()));

        if (summed.has_value())
            expectEquals (*summed, expectedLatency,
                          "aligned impulse should land at the slowest lane latency");

        expectEquals (g.graph.getLatencySamples(), expectedLatency,
                      "the host should report the slowest lane as its latency");
    }
};

static LaneAlignmentThroughGraphTests laneAlignmentThroughGraphTests;
