#pragma once

//==============================================================================
// How many parallel lanes there are.
//
// This number is needed by the graph topology (which groups plugins by lane), the
// settings store (which clamps a lane read from disk and keys one trim per lane),
// the tray and Preferences UI (which offer that many choices), and IconMenu
// (which creates one trim node per lane). It lived in the topology header and was
// separately hardcoded as a run of four menu items in the UI, so raising it meant
// finding every copy.
//==============================================================================
namespace lighthost
{
    /** Highest valid lane index. Lanes are 0 to kMaxLane inclusive. */
    static constexpr int kMaxLane = 3;

    /** Number of lanes, for sizing arrays and loops. */
    static constexpr int kNumLanes = kMaxLane + 1;
}
