#pragma once

#include "Lanes.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// The reserved graph node ids, in one place.
//
// This exists for the same reason Lanes.hpp does. The scheme was written out
// three times -- IconMenu.cpp, OfflineRender.hpp, and again in
// Tests/GraphTopologyTests.cpp -- and the copies had already begun to drift: the
// test one omitted the jlimit clamp that both production copies apply, so the
// helper the tests wire their graphs with could not have caught a clamping
// regression in the code it was testing.
//
// Plugin nodes are allocated from 1 upwards and persisted in the settings file.
// Everything the host inserts itself lives up here, well clear of them, in
// bands so that a new kind of inserted node does not have to be squeezed between
// two existing ones.
//
//   1'000'000        graph audio input
//   1'000'001        graph audio output
//   1'000'010 ..     one lane trim per lane        (Lanes.hpp decides how many)
//   1'000'100 ..     one metering probe per chain position, while the signal
//                    view is open
//==============================================================================
namespace lighthost::nodeids
{
    using NodeID = juce::AudioProcessorGraph::NodeID;

    inline constexpr NodeID input  { 1'000'000u };
    inline constexpr NodeID output { 1'000'001u };

    /** The trim at one lane's summing point. Clamped, because the lane can come
        from the settings file and nothing else validates it.
    */
    [[nodiscard]] inline constexpr NodeID laneGain (int lane) noexcept
    {
        return NodeID { 1'000'010u
                        + static_cast<juce::uint32> (juce::jlimit (0, lighthost::kMaxLane, lane)) };
    }

    /** How many chain positions can carry a metering probe. A longer chain still
        works; it just stops being probed past this point, which is a better
        failure than running the reserved range into something else.
    */
    inline constexpr int maxProbes = 32;

    /** The probe sitting after chain position `index`. */
    [[nodiscard]] inline constexpr NodeID probe (int index) noexcept
    {
        return NodeID { 1'000'100u
                        + static_cast<juce::uint32> (juce::jlimit (0, maxProbes - 1, index)) };
    }
}
