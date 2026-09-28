#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <set>
#include <vector>

//==============================================================================
// Reading what Windows says about this process and the cores it runs on --
// without any of the Windows calls, so it can be tested.
//
// WHY THIS IS SPLIT FROM ProcessQoS.hpp
//
// The same move that produced DeviceTap.hpp out of SignalMetering.hpp: the
// platform call is the only part that cannot be exercised, so it lives apart
// from the interpretation, which is the part that can be wrong.
//
// There is a second reason here. The platform declarations are deliberately NOT
// in this header, so a test that reaches for leaveEcoQoS() fails to COMPILE
// rather than failing to link. A link error in a new test file is the kind of
// thing someone fixes by adding ProcessQoS.cpp to the test target, which would
// pull <windows.h> into LightHostTests and break the Linux build on a commit
// that looked Windows-only.
//
// THE POLARITY TRAP
//
// EfficiencyClass runs the opposite way round to the obvious guess: 0 is the
// LEAST performant, and the P-cores are whatever the highest class present is.
// Getting this backwards would report the audio thread as parked on an E-core
// precisely when it is on a P-core -- the wrong conclusion in the one
// investigation this whole feature exists to support.
//
// A machine with a single class has no P/E split at all, and says so. Reporting
// "efficiency" there would tell the user their audio thread is on an E-core on
// a CPU that has none.
//==============================================================================
namespace lighthost::process
{
    //== Core placement ========================================================
    /** One entry of the system CPU-set table. */
    struct CpuSet
    {
        juce::uint32 id                    = 0;
        juce::uint16 group                 = 0;
        juce::uint8  logicalProcessorIndex = 0;
        juce::uint8  efficiencyClass       = 0;
    };

    struct ProcessorNumber
    {
        juce::uint16 group  = 0;
        juce::uint8  number = 0;
    };

    enum class CoreKind { unknown, uniform, efficiency, performance };

    /** Marks a packed processor number as having actually been sampled.

        Without a validity bit, "never sampled" and "group 0, processor 0" are
        the same value -- and processor 0 is a real core, the one a throttled
        tray process is most likely to be parked on. The two must not collapse.
    */
    inline constexpr juce::uint32 kSampledBit = 0x80000000u;

    /** Packs a processor number so the audio thread can publish it in one
        relaxed atomic store. */
    [[nodiscard]] inline juce::uint32 packProcessor (ProcessorNumber processor) noexcept
    {
        return kSampledBit
             | (static_cast<juce::uint32> (processor.group) << 8)
             | static_cast<juce::uint32> (processor.number);
    }

    [[nodiscard]] inline ProcessorNumber unpackProcessor (juce::uint32 packed) noexcept
    {
        return { static_cast<juce::uint16> ((packed >> 8) & 0xFFFFu),
                 static_cast<juce::uint8>  (packed & 0xFFu) };
    }

    [[nodiscard]] inline bool isSampled (juce::uint32 packed) noexcept
    {
        return (packed & kSampledBit) != 0;
    }

    /** The distinct efficiency classes present, in ascending order. */
    [[nodiscard]] inline std::vector<juce::uint8> efficiencyClasses (const std::vector<CpuSet>& table)
    {
        const std::set<juce::uint8> distinct = [&]
        {
            std::set<juce::uint8> found;
            for (const auto& entry : table)
                found.insert (entry.efficiencyClass);
            return found;
        }();

        return { distinct.begin(), distinct.end() };
    }

    /** Which kind of core a processor number is, per the system CPU-set table.

        See the banner on the polarity. A processor the table does not describe
        is `unknown` rather than a guess: on a machine with more than one
        processor group, matching on the number alone would answer confidently
        about the wrong core.
    */
    // Not noexcept: efficiencyClasses allocates, so a bad_alloc here would
    // become std::terminate rather than something a caller could handle. A
    // promise the code cannot keep is the sort of thing this file exists to
    // avoid.
    [[nodiscard]] inline CoreKind kindOf (const std::vector<CpuSet>& table,
                                          ProcessorNumber processor)
    {
        const auto match = std::find_if (table.begin(), table.end(),
                                         [processor] (const CpuSet& entry)
                                         {
                                             return entry.group == processor.group
                                                 && entry.logicalProcessorIndex == processor.number;
                                         });

        if (match == table.end())
            return CoreKind::unknown;

        const auto classes = efficiencyClasses (table);

        if (classes.size() <= 1)
            return CoreKind::uniform;

        return match->efficiencyClass == classes.back() ? CoreKind::performance
                                                        : CoreKind::efficiency;
    }

    /** What the CPU looks like, for one log line at startup. */
    struct Census
    {
        int total       = 0;
        int classes     = 0;
        int performance = 0;
        int efficiency  = 0;
    };

    [[nodiscard]] inline Census censusOf (const std::vector<CpuSet>& table)
    {
        Census census;
        census.total = static_cast<int> (table.size());

        const auto classes = efficiencyClasses (table);
        census.classes = static_cast<int> (classes.size());

        // A single class is not a split, so counting one side of it would be
        // inventing a distinction the hardware does not make.
        if (census.classes <= 1)
            return census;

        for (const auto& entry : table)
        {
            if (entry.efficiencyClass == classes.back()) ++census.performance;
            else                                         ++census.efficiency;
        }

        return census;
    }

    /** The one- or two-character form, for the Preferences readout. Empty when
        there is nothing worth saying. */
    [[nodiscard]] inline juce::String shortName (CoreKind kind)
    {
        switch (kind)
        {
            case CoreKind::performance: return "P";
            case CoreKind::efficiency:  return "E";
            case CoreKind::unknown:     return "?";
            case CoreKind::uniform:     return {};
        }

        return {};
    }

    // There was a describe(CoreKind) here returning "a performance core" and
    // three friends. Nothing ever called it: the log line and the Preferences
    // readout both want the one-character form, and four user-facing strings
    // that cannot be printed are worse than none.

    [[nodiscard]] inline juce::String describe (const Census& census)
    {
        if (census.total == 0)
            return "cores=unknown";

        auto text = "cores=" + juce::String (census.total)
                  + " classes=" + juce::String (census.classes);

        if (census.classes > 1)
            text += " (" + juce::String (census.performance) + " performance, "
                  + juce::String (census.efficiency) + " efficiency)";

        return text;
    }

    //== EcoQoS ================================================================
    /** What came of asking Windows not to throttle this process.

        `verified` is the field that matters. Without reading the setting back,
        the check is "we called an API" -- which is exactly the class of gate
        this repository keeps finding to have been green for months while being
        incapable of failing.
    */
    struct QosResult
    {
        bool         attempted = false;   ///< false off Windows
        bool         applied   = false;   ///< the call itself succeeded
        bool         verified  = false;   ///< reading it back confirmed it is in force
        juce::uint32 errorCode = 0;
    };

    [[nodiscard]] inline juce::String describe (const QosResult& result)
    {
        if (! result.attempted)
            return "ecoQosOptOut=n/a (not Windows)";

        if (! result.applied)
            return "ecoQosOptOut=failed error=0x"
                 + juce::String::toHexString (static_cast<int> (result.errorCode)).paddedLeft ('0', 8);

        return juce::String ("ecoQosOptOut=applied verified=")
             + (result.verified ? "yes" : "no");
    }

    //== Working set ===========================================================
    /** Evidence for or against the working-set-trimming theory.

        PageFaultCount alone settles nothing: it counts soft faults as well as
        hard ones, cannot tell them apart, and only ever rises -- a measurement
        that cannot fail the hypothesis. What actually evidences trimming is the
        working set having fallen well below its own peak, which is in the same
        struct and costs nothing extra to gather.
    */
    struct MemoryFacts
    {
        bool        available           = false;
        juce::int64 pageFaults          = 0;
        juce::int64 workingSetBytes     = 0;
        juce::int64 peakWorkingSetBytes = 0;
    };

    /** True when the working set has fallen to less than `fraction` of its peak.

        Judged against the peak rather than an absolute, because what counts as
        a small working set depends entirely on the chain that is loaded.
    */
    [[nodiscard]] inline bool looksTrimmed (const MemoryFacts& facts,
                                            double fraction = 0.5) noexcept
    {
        if (! facts.available || facts.peakWorkingSetBytes <= 0)
            return false;

        return static_cast<double> (facts.workingSetBytes)
             < static_cast<double> (facts.peakWorkingSetBytes) * fraction;
    }

    [[nodiscard]] inline juce::String asMegabytes (juce::int64 bytes)
    {
        return juce::String (static_cast<double> (bytes) / (1024.0 * 1024.0), 1) + "MB";
    }

    [[nodiscard]] inline juce::String describe (const MemoryFacts& facts)
    {
        if (! facts.available)
            return "memory=n/a";

        return "pageFaults=" + juce::String (facts.pageFaults)
             + " workingSet=" + asMegabytes (facts.workingSetBytes)
             + " peakWorkingSet=" + asMegabytes (facts.peakWorkingSetBytes)
             + " trimmed=" + (looksTrimmed (facts) ? "yes" : "no");
    }
}
