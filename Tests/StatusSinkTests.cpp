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

        beginTest ("a listener is told about every report, evicting ones included");
        {
            // There is one notifier and one thing it means: a report arrived.
            // The tray tooltip and the Preferences status line both redraw from
            // mostRecent(), so every report has to reach them, including the
            // ones that push an older problem off the end.
            //
            // That last clause is why this fills the sink first. It used to
            // send two reports against a bound of eight, so the eviction its
            // own comment named was never reached: report() could have returned
            // early, or skipped the notify, on the branch that erases and
            // nothing here would have moved. The bound is read from the sink
            // rather than written as 8, so raising it cannot leave this test
            // quietly back under the limit.
            Sink sink;
            int notifications = 0;
            sink.onChange = [&notifications] { ++notifications; };

            constexpr auto bound = static_cast<int> (Sink::kMaxRemembered);

            for (int i = 0; i < bound; ++i)
                sink.report ("problem " + juce::String (i));

            expectEquals (notifications, bound, "a report before the bound went unannounced");
            expectEquals ((int) sink.all().size(), bound);

            // One more: the first report that has to erase before it appends.
            sink.report ("the one that evicts");

            expectEquals (notifications, bound + 1,
                          "the report that evicted an older problem did not reach the listener");
            expectEquals ((int) sink.all().size(), bound,
                          "the bound did not hold once it was actually reached");
            expectEquals (sink.mostRecent(), juce::String ("the one that evicts"));
            expectEquals (sink.all().front().message, juce::String ("problem 1"),
                          "the oldest problem should have been the one dropped");
            expectEquals (sink.totalReported(), bound + 1,
                          "the total must count the dropped report too");
        }
    }
};

static StatusSinkTests statusSinkTests;
