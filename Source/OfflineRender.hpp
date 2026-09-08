#pragma once

#include "GainProcessor.hpp"
#include "GraphTopology.hpp"
#include "Lanes.hpp"
#include "PluginChainStore.hpp"
#include "PluginState.hpp"
#include "PluginStateVault.hpp"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// Pushing a file through the chain, with no audio device involved.
//
// WHY THIS EXISTS
//
// Until now the only way to find out what the chain does to audio was to talk
// into the microphone and capture the virtual cable. That works, but it cannot
// measure latency: two DirectShow devices opened by one process do not start
// sampling at the same instant, and the skew between them is random per run and
// can exceed a quarter of a second. An attempt to measure a plugin's latency
// that way produced an offset of -290 ms -- negative, which no plugin can cause.
// The number was capture skew, not latency, and there is no way to separate them
// from a live capture.
//
// Offline there is no skew. Input sample 0 is input sample 0, so the delay
// between input and output IS the chain's latency, and it can be measured
// exactly. That also makes an A/B meaningful: two plugins can be given
// byte-identical input rather than two different performances.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// It never writes. Not settings, not plugin state, not node ids. It reads the
// chain the user configured and leaves every one of those keys exactly as it
// found them, because a render must never be able to disturb a working setup.
// That is also why this does not reuse IconMenu: constructing one opens audio
// devices, claims the tray icon, and saves plugin state on the way out. This
// borrows the parts that decide things -- the settings store, the topology, the
// state vault -- and none of the parts that own things.
//==============================================================================
namespace lighthost::render
{

struct Result
{
    bool         ok = false;
    juce::String message;

    int    pluginsLoaded    = 0;
    int    pluginsFailed    = 0;
    int    pluginsBypassed  = 0;
    int    connections      = 0;
    int    declaredLatency  = 0;   ///< samples, as the graph reports it
    double sampleRate       = 0.0;
    juce::int64 framesWritten = 0;
};

/** Renders `input` through the chain in `settings` and writes `output`.

    The block size defaults to what the application uses live, so the render
    exercises the same buffering the user actually hears.
*/
[[nodiscard]] inline Result renderFile (juce::PropertySet& settings,
                                        const juce::File& stateDirectory,
                                        const juce::File& input,
                                        const juce::File& output,
                                        int blockSize = 480)
{
    using Graph = juce::AudioProcessorGraph;
    using NodeID = Graph::NodeID;

    Result result;

    static constexpr NodeID kInputNodeId  { 1'000'000u };
    static constexpr NodeID kOutputNodeId { 1'000'001u };
    const auto laneGainNodeId = [] (int lane)
    {
        return NodeID { 1'000'010u + static_cast<juce::uint32> (juce::jlimit (0, kMaxLane, lane)) };
    };

    // ── read the input file ─────────────────────────────────────────────────
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (input));

    if (reader == nullptr)
    {
        result.message = "could not read " + input.getFullPathName();
        return result;
    }

    const auto sampleRate = reader->sampleRate;
    const auto totalFrames = reader->lengthInSamples;
    const int inputChannels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));

    result.sampleRate = sampleRate;

    juce::AudioBuffer<float> source (inputChannels, static_cast<int> (totalFrames));
    reader->read (&source, 0, static_cast<int> (totalFrames), 0, true, inputChannels > 1);

    // ── the chain, exactly as configured ────────────────────────────────────
    const chain::Store store (settings);
    const state::Vault vault (stateDirectory);

    juce::KnownPluginList active;
    if (auto xml = settings.getXmlValue ("pluginListActive"))
        active.recreateFromXml (*xml);

    const auto types = active.getTypes();

    std::vector<std::pair<int, juce::PluginDescription>> ordered;
    ordered.reserve (static_cast<size_t> (types.size()));
    for (const auto& pd : types)
        ordered.emplace_back (store.readOrder (pd), pd);

    const auto sorted = chain::Store::sortByOrder (std::move (ordered));

    // ── build the graph ─────────────────────────────────────────────────────
    Graph graph;
    graph.setNonRealtime (true);

    graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (
                       Graph::AudioGraphIOProcessor::audioInputNode), kInputNodeId);
    graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (
                       Graph::AudioGraphIOProcessor::audioOutputNode), kOutputNodeId);

    for (int lane = 0; lane < kNumLanes; ++lane)
        if (auto node = graph.addNode (std::make_unique<gain::Processor>(), laneGainNodeId (lane)))
            if (auto* proc = dynamic_cast<gain::Processor*> (node->getProcessor()))
                proc->setGainDb (store.readLaneGainDb (lane));

    juce::AudioPluginFormatManager plugins;

    // JUCE 9 deleted AudioPluginFormatManager::addDefaultFormats() in favour of
    // these free functions. The headless variant is the right one here: a render
    // never opens a plugin editor, and the formats it registers say so.
    addDefaultFormatsToManager (plugins);

    // Instantiated synchronously. The live path loads asynchronously to keep the
    // message loop pumping, which matters for a UI and not at all here; blocking
    // is simpler and removes the need to wait for a completion callback.
    juce::uint32 nextNodeId = 1;
    std::vector<topology::NodeFacts> nodes;

    for (const auto& pd : sorted)
    {
        juce::String error;
        auto instance = plugins.createPluginInstance (pd, sampleRate, blockSize, error);

        if (instance == nullptr)
        {
            ++result.pluginsFailed;
            juce::Logger::writeToLog ("Render: could not load " + pd.name + ": " + error);
            continue;
        }

        instance->setNonRealtime (true);

        const auto stored = vault.read (chain::Store::identityOf (pd));
        if (stored.getSize() > 0)
            (void) state::restoreInto (*instance, stored);

        const NodeID nodeId { nextNodeId++ };
        auto node = graph.addNode (std::move (instance), nodeId);

        if (node == nullptr)
        {
            ++result.pluginsFailed;
            continue;
        }

        const bool bypassed = store.readBypassed (pd);
        node->setBypassed (bypassed);

        if (bypassed)
        {
            ++result.pluginsBypassed;
            continue;   // wired around, exactly as the live path does
        }

        ++result.pluginsLoaded;

        auto* proc = node->getProcessor();
        topology::NodeFacts facts;
        facts.nodeId            = nodeId;
        facts.lane              = juce::jlimit (0, kMaxLane, store.readLane (pd));
        facts.numInputChannels  = proc->getTotalNumInputChannels();
        facts.numOutputChannels = proc->getTotalNumOutputChannels();
        nodes.push_back (facts);
    }

    // ── prepare, then wire ──────────────────────────────────────────────────
    graph.setPlayConfigDetails (inputChannels, 2, sampleRate, blockSize);
    graph.prepareToPlay (sampleRate, blockSize);

    topology::Layout layout;
    layout.inputNodeId  = kInputNodeId;
    layout.outputNodeId = kOutputNodeId;
    layout.nodes        = nodes;

    if (auto* in = graph.getNodeForId (kInputNodeId))
        layout.inputNodeChannels = in->getProcessor()->getTotalNumOutputChannels();
    if (auto* out = graph.getNodeForId (kOutputNodeId))
        layout.outputNodeChannels = out->getProcessor()->getTotalNumInputChannels();

    for (int lane = 0; lane < kNumLanes; ++lane)
        if (graph.getNodeForId (laneGainNodeId (lane)) != nullptr)
            layout.laneGainNodeIds[static_cast<size_t> (lane)] = laneGainNodeId (lane);

    const auto desired = topology::buildConnections (layout);
    (void) topology::applyConnections (graph, desired);
    graph.rebuild();

    result.connections     = static_cast<int> (desired.size());
    result.declaredLatency = graph.getLatencySamples();

    // ── push the file through ───────────────────────────────────────────────
    // The tail is followed for the declared latency plus one block, so a plugin
    // that delays audio does not have its last words cut off by the render
    // finishing before they arrive.
    const auto tail = result.declaredLatency + blockSize;
    const auto framesToRender = totalFrames + tail;

    const int graphChannels = juce::jmax (inputChannels, 2);
    juce::AudioBuffer<float> block (graphChannels, blockSize);
    juce::AudioBuffer<float> rendered (2, static_cast<int> (framesToRender));
    rendered.clear();

    juce::MidiBuffer midi;
    juce::int64 position = 0;

    while (position < framesToRender)
    {
        const int thisBlock = static_cast<int> (juce::jmin (static_cast<juce::int64> (blockSize),
                                                            framesToRender - position));
        block.clear();

        for (int ch = 0; ch < inputChannels; ++ch)
        {
            const auto available = juce::jmin (static_cast<juce::int64> (thisBlock),
                                               juce::jmax<juce::int64> (0, totalFrames - position));
            if (available > 0)
                block.copyFrom (ch, 0, source, ch, static_cast<int> (position),
                                static_cast<int> (available));
        }

        midi.clear();
        graph.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            rendered.copyFrom (ch, static_cast<int> (position),
                               block, juce::jmin (ch, graphChannels - 1), 0, thisBlock);

        position += thisBlock;
    }

    graph.releaseResources();

    // ── write it out ────────────────────────────────────────────────────────
    output.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (output.createOutputStream());

    if (stream == nullptr)
    {
        result.message = "could not write " + output.getFullPathName();
        return result;
    }

    juce::WavAudioFormat wav;

    // The options-taking overload, not the deprecated positional one: it takes
    // ownership of the stream through the unique_ptr rather than by raw pointer
    // and a release() the caller has to remember.
    std::unique_ptr<juce::OutputStream> asStream (std::move (stream));

    auto writer = wav.createWriterFor (asStream,
                                       juce::AudioFormatWriterOptions{}
                                           .withSampleRate (sampleRate)
                                           .withNumChannels (2)
                                           .withBitsPerSample (24));

    if (writer == nullptr)
    {
        result.message = "could not create a WAV writer";
        return result;
    }

    writer->writeFromAudioSampleBuffer (rendered, 0, rendered.getNumSamples());
    writer.reset();

    result.framesWritten = rendered.getNumSamples();
    result.ok = true;
    result.message = "rendered " + juce::String (result.pluginsLoaded) + " active plugin(s), "
                   + juce::String (result.pluginsBypassed) + " bypassed, "
                   + juce::String (result.connections) + " connections, declared latency "
                   + juce::String (result.declaredLatency) + " samples";
    return result;
}

/** True when --render appears on the command line. */
[[nodiscard]] inline bool isRequested (const juce::StringArray& params)
{
    return params.contains ("--render") || params.contains ("-render");
}

/** The two file arguments following --render, or empty strings when absent. */
inline void parseArguments (const juce::StringArray& params,
                            juce::String& inPath,
                            juce::String& outPath)
{
    for (int i = 0; i < params.size(); ++i)
    {
        if (params[i] != "--render" && params[i] != "-render")
            continue;

        if (i + 1 < params.size()) inPath  = params[i + 1].unquoted();
        if (i + 2 < params.size()) outPath = params[i + 2].unquoted();
        return;
    }
}

} // namespace lighthost::render
