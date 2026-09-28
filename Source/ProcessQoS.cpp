#include "ProcessQoS.hpp"

#include <juce_core/juce_core.h>

// After the JUCE include, never before. juce_BasicNativeHeaders.h has already
// set NOMINMAX, STRICT, WIN32_LEAN_AND_MEAN and the SDK version macros by this
// point, so including <windows.h> bare picks them up -- and defining them here
// would be a -Wmacro-redefinition under Clang. Same arrangement, and the same
// reason, as IconMenu.cpp:17-20; the rationale is recorded at CMakeLists.txt
// :174-192.
//
// PROCESS_POWER_THROTTLING_STATE is gated on NTDDI_VERSION >= NTDDI_WIN10_RS3,
// and the target already defines NTDDI_VERSION=0x0A00000C, so no new define is
// needed here.
#if JUCE_WINDOWS
 #include <windows.h>
 #include <psapi.h>
#endif

namespace lighthost::process
{
#if JUCE_WINDOWS
namespace
{
    /** Walks the variable-length CPU-set records.

        By each record's own Size field, not by sizeof(SYSTEM_CPU_SET_INFORMATION):
        the structure is versioned and a future Windows may make it longer, at
        which point striding by the compile-time size would read every entry
        after the first from the wrong offset.
    */
    [[nodiscard]] std::vector<CpuSet> queryCpuSets()
    {
        ULONG required = 0;

        // Expected to fail with ERROR_INSUFFICIENT_BUFFER; the output parameter
        // is the point of the call.
        GetSystemCpuSetInformation (nullptr, 0, &required, GetCurrentProcess(), 0);

        if (required == 0)
            return {};

        // uint64 storage rather than char, so the buffer is 8-byte aligned for
        // the records reinterpreted out of it.
        std::vector<juce::uint64> storage ((required + sizeof (juce::uint64) - 1)
                                             / sizeof (juce::uint64));
        auto* base = reinterpret_cast<juce::uint8*> (storage.data());

        if (! GetSystemCpuSetInformation (reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION> (base),
                                          required, &required, GetCurrentProcess(), 0))
            return {};

        std::vector<CpuSet> table;

        for (ULONG offset = 0; offset < required;)
        {
            const auto* record = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*> (base + offset);

            if (record->Size == 0)
                break;

            if (record->Type == CpuSetInformation)
                table.push_back ({ static_cast<juce::uint32> (record->CpuSet.Id),
                                   static_cast<juce::uint16> (record->CpuSet.Group),
                                   static_cast<juce::uint8>  (record->CpuSet.LogicalProcessorIndex),
                                   static_cast<juce::uint8>  (record->CpuSet.EfficiencyClass) });

            offset += record->Size;
        }

        return table;
    }
}
#endif

QosResult leaveEcoQoS()
{
    QosResult result;

#if JUCE_WINDOWS
    result.attempted = true;

    // Brace-initialised and then assigned field by field: a partial
    // brace-init list trips -Wmissing-field-initializers, and the struct has
    // members this code has no business naming.
    PROCESS_POWER_THROTTLING_STATE request {};
    request.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    request.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    request.StateMask   = 0;   // 0 with the bit in ControlMask means "do not throttle"

    if (! SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling,
                                 &request, sizeof (request)))
    {
        result.errorCode = static_cast<juce::uint32> (GetLastError());
        return result;
    }

    result.applied = true;

    // Read it back. Without this the check is "we called an API" -- and a
    // machine policy or a job object can refuse the request while the call
    // itself succeeds.
    PROCESS_POWER_THROTTLING_STATE inForce {};
    inForce.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;

    if (GetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling,
                               &inForce, sizeof (inForce)))
        result.verified = (inForce.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0
                       && (inForce.StateMask   & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) == 0;
#endif

    return result;
}

const std::vector<CpuSet>& systemCpuSets()
{
#if JUCE_WINDOWS
    // Function-local static: initialised once, thread-safe by the standard, and
    // holding PODs only so there is nothing for the leak detector to see.
    static const std::vector<CpuSet> table = queryCpuSets();
    return table;
#else
    static const std::vector<CpuSet> empty;
    return empty;
#endif
}

juce::uint32 currentProcessorPacked() noexcept
{
#if JUCE_WINDOWS
    PROCESSOR_NUMBER processor {};
    GetCurrentProcessorNumberEx (&processor);

    return packProcessor ({ static_cast<juce::uint16> (processor.Group),
                            static_cast<juce::uint8>  (processor.Number) });
#else
    return 0;
#endif
}

MemoryFacts memoryFacts()
{
    MemoryFacts facts;

#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS counters {};
    counters.cb = sizeof (counters);

    if (GetProcessMemoryInfo (GetCurrentProcess(), &counters, sizeof (counters)))
    {
        facts.available           = true;
        facts.pageFaults          = static_cast<juce::int64> (counters.PageFaultCount);
        facts.workingSetBytes     = static_cast<juce::int64> (counters.WorkingSetSize);
        facts.peakWorkingSetBytes = static_cast<juce::int64> (counters.PeakWorkingSetSize);
    }
#endif

    return facts;
}
}
