#pragma once

#include <map>

//==============================================================================
// Plugin Delay Compensation arithmetic, extracted from IconMenu::reconnectGraph
// so it can be unit tested without standing up an AudioProcessorGraph, a
// device manager, and a settings file.
//
// Deliberately free of JUCE types and of any notion of nodes or connections:
// input is "how much latency does each lane carry", output is "how many samples
// of delay must each lane be padded with". reconnectGraph() owns everything else.
//==============================================================================
namespace lighthost::pdc
{
    /** Highest per-lane latency in the map. Returns 0.0 for an empty map.

        Note the floor at 0.0: a lane whose plugins report a net negative
        latency does not drag the maximum below zero.
    */
    [[nodiscard]] inline double maxLaneLatency (const std::map<int, double>& perLaneLatency)
    {
        double maxLatency = 0.0;

        for (const auto& [lane, latency] : perLaneLatency)
            if (latency > maxLatency)
                maxLatency = latency;

        return maxLatency;
    }

    /** For each lane, the delay in samples to insert after that lane's last node
        so every lane arrives sample-aligned at the summing point.

        The lane carrying the most latency gets 0; the rest get the difference.
        Fractional latencies truncate toward zero, matching the original inline
        implementation. Callers should treat a returned 0 as "insert no delay
        node at all" rather than "insert a zero-length one".
    */
    [[nodiscard]] inline std::map<int, int> computeLaneDelays (const std::map<int, double>& perLaneLatency)
    {
        const double maxLatency = maxLaneLatency (perLaneLatency);

        std::map<int, int> delays;

        for (const auto& [lane, latency] : perLaneLatency)
        {
            const int diff = static_cast<int> (maxLatency - latency);
            delays[lane] = diff > 0 ? diff : 0;
        }

        return delays;
    }
}
