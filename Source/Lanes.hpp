#pragma once

//==============================================================================
// How many parallel lanes there are.
//
// This number is needed by the graph topology (which groups plugins by lane), the
// settings store (which clamps a lane read from disk and keys one trim per lane),
// the Preferences UI (which offers that many choices, both as trim sliders and as
// the per-plugin lane menu), and IconMenu (which creates one trim node per lane).
//
// It lived in the topology header and was separately hardcoded as a run of four
// menu items in the UI. That run outlived the first attempt to retire it by two
// releases -- the constant landed, the menu did not -- so anyone raising kMaxLane
// got trims, settings keys and graph nodes for the new lanes and no way to assign
// a plugin to one. Both now loop over kNumLanes.
//
// The tray menu offers no lane choice at all, which an earlier version of this
// comment claimed it did.
//==============================================================================
namespace lighthost
{
    /** Highest valid lane index. Lanes are 0 to kMaxLane inclusive. */
    static constexpr int kMaxLane = 3;

    /** Number of lanes, for sizing arrays and loops. */
    static constexpr int kNumLanes = kMaxLane + 1;
}
