#pragma once

#include "ProcessFacts.hpp"

//==============================================================================
// The four Windows calls behind ProcessFacts.hpp.
//
// Everything here is a query or a one-shot setting; all of the interpretation
// is next door, where it can be tested. This header exists so that <windows.h>
// stays inside ProcessQoS.cpp: DeviceTap.hpp includes this, and DeviceTap.hpp
// reaches IconMenu.hpp and thence most of the GUI.
//
// ProcessQoS.cpp is compiled into the application ONLY. It must never be added
// to LightHostTests -- see the banner in ProcessFacts.hpp.
//==============================================================================
namespace lighthost::process
{
    /** Opts this process out of EcoQoS, and reads the setting back.

        Windows parks a process with no visible window on efficiency cores and
        clock-gates it. For a tray-resident audio host that is not a saving, it
        is a dropout: on a hybrid CPU, "efficiency mode" means the audio thread
        is explicitly directed away from the performance cores, and the MMCSS
        "Pro Audio" registration JUCE already makes for the WASAPI thread
        cannot override a process-level power-throttling directive.

        The cost of opting out is close to nothing, because throttling only
        bites a process that actually burns CPU and this one is near-idle while
        it waits.

        Windows may legitimately refuse -- inside a job object, or under machine
        policy. That is logged and otherwise ignored: there is nothing the user
        can do about it, so it does not reach the status sink.
    */
    [[nodiscard]] QosResult leaveEcoQoS();

    /** The system CPU-set table, queried once and cached for the process.

        CPU hot-add is not handled. It does not happen on hardware this ships
        to, and a table that could change under a lock-free audio-thread reader
        would be a real cost for a diagnostic.
    */
    [[nodiscard]] const std::vector<CpuSet>& systemCpuSets();

    /** The processor the calling thread is on, packed by packProcessor.

        Zero -- that is, not sampled -- when the platform cannot say. Cheap
        enough to call from the audio thread: on Windows this reads the TEB
        rather than making a syscall.
    */
    [[nodiscard]] juce::uint32 currentProcessorPacked() noexcept;

    /** Page faults and the working set, for the dropout log line. */
    [[nodiscard]] MemoryFacts memoryFacts();
}
