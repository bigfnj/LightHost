#pragma once

#include "GainProcessor.hpp"
#include "GraphTopology.hpp"
#include "Lanes.hpp"
#include "PluginChainStore.hpp"
#include "PluginState.hpp"
#include "PluginStateVault.hpp"

#include <cmath>

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

    /// One line per parameter of every active plugin, read back after its
    /// stored state was restored. A measurement is only as trustworthy as the
    /// settings it ran under, and reading those out of the plugin beats
    /// reading them off a screenshot.
    juce::StringArray parameterReport;

    /// Wall-clock time divided by audio duration. About 1.0 for a paced render
    /// that kept up, far below 1.0 for --fast, and above 1.0 when the machine
    /// could not keep pace -- which is the case whose results cannot be trusted.
    double realtimeFactor = 0.0;

    /// Blocks that were already late when they started, so pacing could not
    /// slow down for them. Any non-zero count means a worker-thread plugin was
    /// starved for that many blocks, exactly the failure pacing exists to avoid.
    int blocksBehind = 0;
    int blocksTotal  = 0;

    /// Whether pacing was in effect at all. Distinguishes "paced and kept up"
    /// from "never paced", which a zero blocksBehind cannot.
    bool wasPaced = false;
};

/** Renders `input` through the chain in `settings` and writes `output`.

    The block size defaults to what the application uses live, so the render
    exercises the same buffering the user actually hears.
*/
[[nodiscard]] inline Result renderFile (juce::PropertySet& settings,
                                        const juce::File& stateDirectory,
                                        const juce::File& input,
                                        const juce::File& output,
                                        int blockSize = 480,
                                        const juce::StringArray& overrides = {},
                                        bool paced = true,
                                        const juce::StringArray& chainOverride = {},
                                        const std::function<void (int)>& wait = {})
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

    std::vector<std::pair<int, juce::PluginDescription>> ordered;

    if (chainOverride.isEmpty())
    {
        juce::KnownPluginList active;
        if (auto xml = settings.getXmlValue ("pluginListActive"))
            active.recreateFromXml (*xml);

        const auto types = active.getTypes();
        ordered.reserve (static_cast<size_t> (types.size()));

        for (const auto& pd : types)
            ordered.emplace_back (store.readOrder (pd), pd);
    }
    else
    {
        // An explicit chain is looked up in everything that has been scanned,
        // not in the saved chain, so a plugin can be A/B tested without being
        // added to the user's live setup first. Comparing two denoisers should
        // not require reconfiguring the thing you are measuring.
        juce::KnownPluginList known;
        if (auto xml = settings.getXmlValue ("pluginList"))
            known.recreateFromXml (*xml);

        const auto types = known.getTypes();
        int position = 0;

        for (const auto& wanted : chainOverride)
        {
            bool found = false;

            for (const auto& pd : types)
            {
                if (! pd.name.containsIgnoreCase (wanted))
                    continue;

                ordered.emplace_back (position++, pd);
                found = true;
                break;
            }

            if (! found)
            {
                result.message = "no scanned plugin matches \"" + wanted + "\"";
                return result;
            }
        }
    }

    const auto sorted = chain::Store::sortByOrder (std::move (ordered));

    // ── build the graph ─────────────────────────────────────────────────────
    Graph graph;
    // Paced renders leave the graph in real-time mode on purpose. A plugin that
    // runs its model on a worker thread behaves completely differently when the
    // audio thread is not waiting for it, so measuring what the user actually
    // hears means feeding blocks at the rate the user's hardware would.
    graph.setNonRealtime (! paced);

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

        instance->setNonRealtime (! paced);

        const auto stored = vault.read (chain::Store::identityOf (pd));
        if (stored.getSize() > 0)
            (void) state::restoreInto (*instance, stored);

        // Overrides land after the stored state, so they win. Sweeping one knob
        // across its range is the only reliable way to find out what it does
        // when the documentation is not to hand, and doing it here means the
        // user's saved configuration is never touched to run the experiment.
        //
        // "Name=text" asks the plugin to interpret the text, which is what you
        // want for a value in real units like -18. "Name@0.25" sets the raw
        // normalised position instead, for plugins that cannot parse their own
        // display strings.
        for (const auto& spec : overrides)
        {
            // "=" binds at the FIRST occurrence and wins outright, so a value
            // containing "=" or "@" cannot hijack the split ("Note=A@440" used to
            // parse as the parameter "Note=A"). Position mode applies only when
            // there is no "=" at all.
            const auto eqPos      = spec.indexOfChar ('=');
            const bool byPosition = eqPos < 0;
            const auto sepPos     = byPosition ? spec.indexOfChar ('@') : eqPos;

            if (sepPos <= 0)
            {
                juce::Logger::writeToLog ("Render: ignoring malformed --param " + spec);
                continue;
            }

            const auto wanted = spec.substring (0, sepPos).trim();
            const auto text   = spec.substring (sepPos + 1).trim();
            int matches = 0;

            // Every parameter of that name is set, not just the first: plugins
            // really do expose two parameters both called "Bypass", and picking
            // one of them would be a coin toss.
            for (auto* param : instance->getParameters())
            {
                if (param == nullptr || ! param->getName (64).equalsIgnoreCase (wanted))
                    continue;

                const auto norm = byPosition ? text.getFloatValue()
                                             : param->getValueForText (text);

                // getValueForText is the plugin's own parser and may hand back
                // NaN for text it cannot read. juce::jlimit passes NaN through
                // untouched -- both of its comparisons are false -- so without
                // this the NaN reaches the DSP and the render still says OK.
                if (std::isnan (norm) || std::isinf (norm))
                {
                    juce::Logger::writeToLog ("Render: plugin could not parse \"" + text
                                              + "\" for " + wanted + "; left unchanged");
                    continue;
                }

                param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, norm));
                ++matches;
            }

            if (matches == 0)
                juce::Logger::writeToLog ("Render: no parameter named \"" + wanted + "\"");
        }

        const NodeID nodeId { nextNodeId++ };
        auto node = graph.addNode (std::move (instance), nodeId);

        if (node == nullptr)
        {
            ++result.pluginsFailed;
            continue;
        }

        // A plugin named explicitly on the command line is being measured, so a
        // stored bypass flag must not silently switch it off.
        const bool bypassed = chainOverride.isEmpty() && store.readBypassed (pd);
        node->setBypassed (bypassed);

        if (bypassed)
        {
            ++result.pluginsBypassed;
            continue;   // wired around, exactly as the live path does
        }

        ++result.pluginsLoaded;

        auto* proc = node->getProcessor();

        for (const auto* param : proc->getParameters())
            if (param != nullptr && param->getName (64).isNotEmpty())
                result.parameterReport.add (pd.name + " / " + param->getName (64)
                                            + " = " + param->getCurrentValueAsText());

        topology::NodeFacts facts;
        facts.nodeId            = nodeId;
        facts.lane              = chainOverride.isEmpty()
                                    ? juce::jlimit (0, kMaxLane, store.readLane (pd))
                                    : 0;
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
    result.wasPaced = paced;
    const auto wallStartMs = juce::Time::getMillisecondCounterHiRes();

    while (position < framesToRender)
    {
        // Hold the render to real time when asked, so a worker-thread plugin
        // gets the same wall-clock budget per block that it would live. Sleeping
        // before the block, not after, means the plugin's thread has already had
        // its time when processBlock arrives.
        if (paced && position > 0)
        {
            // Cumulative, not per block: sleeping a fixed amount each time would
            // let error pile up over a long file.
            const auto audioMs   = 1000.0 * (double) position / sampleRate;
            const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - wallStartMs;

            ++result.blocksTotal;

            if (audioMs > elapsedMs)
            {
                const auto ms = juce::roundToInt (audioMs - elapsedMs);

                if (wait)
                    wait (ms);
                else
                    juce::Thread::sleep (ms);
            }
            else if (position > static_cast<juce::int64> (sampleRate))
            {
                // Only counted past the first second, because loading a neural
                // model takes a moment and every block during that catch-up
                // would otherwise be reported as starvation. A warning that
                // fires on healthy runs is a warning people learn to ignore.
                ++result.blocksBehind;
            }
        }

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

    {
        const auto wallMs  = juce::Time::getMillisecondCounterHiRes() - wallStartMs;
        const auto audioMs = 1000.0 * (double) framesToRender / sampleRate;
        result.realtimeFactor = audioMs > 0.0 ? wallMs / audioMs : 0.0;
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

/** Plugin names from `--chain "A,B"` (repeatable), in the order given. */
[[nodiscard]] inline juce::StringArray parseChain (const juce::StringArray& params)
{
    juce::StringArray out;

    for (int i = 0; i < params.size(); ++i)
        if ((params[i] == "--chain" || params[i] == "-chain") && i + 1 < params.size())
            out.addArray (juce::StringArray::fromTokens (params[i + 1].unquoted(), ",", ""));

    out.trim();
    out.removeEmptyStrings();
    return out;
}

/** True unless `--fast` was given.

    Pacing is the default because the alternative is silently wrong. A plugin
    that runs its model on a worker thread cannot keep up with a render going
    thirty times faster than real time, so it falls back to passing the dry
    signal through -- and the render still reports success, having measured
    nothing. That mistake cost an afternoon and produced a confident, entirely
    wrong verdict on a plugin that turned out to remove 25 dB of keyboard noise.
    A render that takes as long as the audio is a small price for one that is
    telling the truth. `--fast` remains available for plugins known to be
    synchronous, where it is roughly thirty times quicker.
*/
[[nodiscard]] inline bool isPaced (const juce::StringArray& params)
{
    return ! (params.contains ("--fast") || params.contains ("-fast"));
}

/** Every `--param NAME=VALUE` (or `NAME@0.5`) given on the command line. */
[[nodiscard]] inline juce::StringArray parseOverrides (const juce::StringArray& params)
{
    juce::StringArray out;

    for (int i = 0; i < params.size(); ++i)
        if ((params[i] == "--param" || params[i] == "-param") && i + 1 < params.size())
            out.add (params[i + 1].unquoted());

    return out;
}

} // namespace lighthost::render
