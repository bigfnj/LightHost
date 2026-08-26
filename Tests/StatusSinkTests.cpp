#include "../Source/StatusSink.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// The status sink: where failures go so they can be shown rather than only
// logged.
//==============================================================================
class StatusSinkTests final : public juce::UnitTest
{
public:
    StatusSinkTests()
        : juce::UnitTest ("Status sink", "Status") {}

    void runTest() override
    {
        using lighthost::status::Sink;

        beginTest ("a fresh sink has nothing to report");
        {
            const Sink sink;

            expect (! sink.hasProblem());
            expect (sink.mostRecent().isEmpty());
            expectEquals (sink.totalReported(), 0);
        }

        beginTest ("the most recent problem is the one shown");
        {
            Sink sink;
            sink.report ("settings could not be saved");
            sink.report ("audio device would not open");

            expect (sink.hasProblem());
            expectEquals (sink.mostRecent(), juce::String ("audio device would not open"));
            expectEquals (sink.totalReported(), 2);
        }

        beginTest ("history is bounded, and the newest survives");
        {
            // A status surface, not a history. A plugin failing to load in a loop
            // must not grow this without limit.
            Sink sink;

            for (int i = 0; i < (int) Sink::kMaxRemembered * 3; ++i)
                sink.report ("problem " + juce::String (i));

            expectEquals ((int) sink.all().size(), (int) Sink::kMaxRemembered);
            expectEquals (sink.mostRecent(),
                          juce::String ("problem " + juce::String ((int) Sink::kMaxRemembered * 3 - 1)));
            expectEquals (sink.totalReported(), (int) Sink::kMaxRemembered * 3,
                          "the total should count everything, not just what is remembered");
        }

        beginTest ("clearing forgets the problems but not that there were some");
        {
            Sink sink;
            sink.report ("something");
            sink.clear();

            expect (! sink.hasProblem());
            expect (sink.mostRecent().isEmpty());
            expectEquals (sink.totalReported(), 1,
                          "a cleared sink should still admit something happened");
        }

        beginTest ("a listener is told when something changes");
        {
            Sink sink;
            int notifications = 0;
            sink.onChange = [&notifications] { ++notifications; };

            sink.report ("one");
            expectEquals (notifications, 1);

            sink.clear();
            expectEquals (notifications, 2);

            // Clearing an already-empty sink changes nothing, so it notifies
            // nothing: a UI should not be asked to redraw for no reason.
            sink.clear();
            expectEquals (notifications, 2);
        }
    }
};

static StatusSinkTests statusSinkTests;
