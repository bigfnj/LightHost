#include "../Source/ProcessFacts.hpp"

#include <juce_core/juce_core.h>

#include <vector>

//==============================================================================
// Interpreting what Windows reports about this process and its cores.
//
// The load-bearing property is the polarity of EfficiencyClass. It runs the
// opposite way round to the obvious guess -- 0 is the LEAST performant, and the
// performance cores are whatever the highest class present is. Inverting it
// would report the audio thread as parked on an efficiency core precisely when
// it is on a performance one, which is the wrong conclusion in the single
// investigation this feature exists to support.
//
// The second is that a CPU with one class has no split to report. Saying
// "efficiency core" on a machine that has none is worse than saying nothing.
//==============================================================================
namespace
{
    using namespace lighthost::process;

    [[nodiscard]] CpuSet core (juce::uint16 group,
                               juce::uint8 number,
                               juce::uint8 efficiencyClass)
    {
        return { static_cast<juce::uint32> (number), group, number, efficiencyClass };
    }

    //==========================================================================
    class ProcessFactsTests final : public juce::UnitTest
    {
    public:
        ProcessFactsTests()
            : juce::UnitTest ("Process QoS facts", "ProcessFacts") {}

        void runTest() override
        {
            beginTest ("the most performant class is the highest number, not the lowest");
            {
                // FAILS IF: the polarity is inverted. See the banner -- this is
                // the assertion the whole header exists to protect.
                const std::vector<CpuSet> table { core (0, 0, 0), core (0, 1, 0),
                                                  core (0, 2, 1), core (0, 3, 1) };

                expect (kindOf (table, { 0, 2 }) == CoreKind::performance,
                        "a core in the highest efficiency class was reported as an "
                        "efficiency core, which inverts every conclusion drawn from it");
                expect (kindOf (table, { 0, 0 }) == CoreKind::efficiency);
            }

            beginTest ("a machine with one class has no efficiency cores");
            {
                // FAILS IF: every core on a homogeneous CPU reads as an E-core.
                const std::vector<CpuSet> table { core (0, 0, 0), core (0, 1, 0), core (0, 2, 0) };

                expect (kindOf (table, { 0, 1 }) == CoreKind::uniform,
                        "a CPU with a single core class is being described as having a split");

                const auto census = censusOf (table);
                expectEquals (census.classes, 1);
                expectEquals (census.performance, 0);
                expectEquals (census.efficiency, 0);
                expectEquals (census.total, 3);
            }

            beginTest ("a three-class machine has one performance class and the rest are efficiency");
            {
                // The real case on this hardware: P-cores, E-cores and LP-E
                // cores. Only the top class is a performance core.
                const std::vector<CpuSet> table { core (0, 0, 0), core (0, 1, 1), core (0, 2, 2) };

                expect (kindOf (table, { 0, 2 }) == CoreKind::performance);
                expect (kindOf (table, { 0, 1 }) == CoreKind::efficiency);
                expect (kindOf (table, { 0, 0 }) == CoreKind::efficiency);

                const auto census = censusOf (table);
                expectEquals (census.classes, 3);
                expectEquals (census.performance, 1);
                expectEquals (census.efficiency, 2);
            }

            beginTest ("a processor the table does not describe is unknown, not a guess");
            {
                const std::vector<CpuSet> table { core (0, 0, 0), core (0, 1, 1) };
                expect (kindOf (table, { 0, 9 }) == CoreKind::unknown);
            }

            beginTest ("processor numbers are matched within their group");
            {
                // FAILS IF: a machine with more than 64 logical processors
                // reports a core from the wrong group, confidently.
                const std::vector<CpuSet> table { core (0, 3, 1), core (1, 3, 0), core (1, 4, 1) };

                expect (kindOf (table, { 0, 3 }) == CoreKind::performance);
                expect (kindOf (table, { 1, 3 }) == CoreKind::efficiency,
                        "a processor number was matched across processor groups");
            }

            beginTest ("an unsampled processor is not processor zero");
            {
                // FAILS IF: a tray app that never sampled reports the core it is
                // most likely to actually have been parked on.
                expect (! isSampled (0), "a never-sampled reading reads as a real core");
                expect (isSampled (packProcessor ({ 0, 0 })));

                const auto round = unpackProcessor (packProcessor ({ 0, 0 }));
                expectEquals (static_cast<int> (round.group), 0);
                expectEquals (static_cast<int> (round.number), 0);
            }

            beginTest ("pack and unpack round-trip across the range");
            {
                for (const int group : { 0, 1, 7, 63 })
                {
                    for (const int number : { 0, 1, 31, 255 })
                    {
                        const ProcessorNumber processor { static_cast<juce::uint16> (group),
                                                          static_cast<juce::uint8>  (number) };
                        const auto round = unpackProcessor (packProcessor (processor));

                        expectEquals (static_cast<int> (round.group), group);
                        expectEquals (static_cast<int> (round.number), number);
                    }
                }
            }

            beginTest ("an empty table interprets nothing");
            {
                const std::vector<CpuSet> table;
                expect (kindOf (table, { 0, 0 }) == CoreKind::unknown);
                expectEquals (censusOf (table).total, 0);
                expect (describe (censusOf (table)).contains ("unknown"));
            }

            beginTest ("a QoS result that was never attempted does not read as applied");
            {
                // FAILS IF: an opt-out Windows refused logs as if it succeeded,
                // which would make the whole line a gate that cannot fail.
                expect (describe (QosResult {}).contains ("n/a"));
                expect (! describe (QosResult {}).contains ("applied"));

                QosResult failed;
                failed.attempted = true;
                failed.errorCode = 0x57;
                expect (describe (failed).contains ("failed"));
                expect (describe (failed).contains ("00000057"),
                        "the error code is not in the log line, so a refusal cannot be diagnosed");

                QosResult unverified;
                unverified.attempted = true;
                unverified.applied   = true;
                QosResult verified = unverified;
                verified.verified  = true;

                expect (describe (unverified) != describe (verified),
                        "an opt-out that could not be read back reads the same as one "
                        "confirmed to be in force");
            }

            beginTest ("the memory line reports the figures and interprets none of them");
            {
                // There was a trimmed=yes/no here, derived from the working set
                // against its own peak. It read yes on every healthy run,
                // because the peak is a one-off model load, and it produced a
                // confident wrong diagnosis. The figures are reported raw now.
                const MemoryFacts facts { true, 4312, 38 * 1024 * 1024, 52 * 1024 * 1024 };
                const auto text = describe (facts);

                expect (! text.contains ("trimmed"),
                        "a predicate measured against a load-transient peak is back, and "
                        "it is true of every healthy run");

                for (const auto* key : { "pageFaults=", "workingSet=", "peakWorkingSet=" })
                    expect (text.contains (key),
                            juce::String ("the memory line lost its ") + key + " field");

                expect (describe (MemoryFacts {}).contains ("n/a"));
            }
        }
    };

    static ProcessFactsTests processFactsTests;
}
