#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

//==============================================================================
// Somewhere for failures to go, other than nowhere.
//
// Light Host discarded most of its error returns. AudioDeviceManager::initialise
// returns a description of why it could not open a device; that string was thrown
// away. juce::PropertiesFile::saveIfNeeded returns false when the settings file
// could not be written; every one of the thirteen call sites ignored it. A user
// whose disk was full, or whose settings file was locked by a backup agent, lost
// every chain edit they made and had no way to know: the app looked like it was
// working.
//
// Logging alone is not enough. The log is a file the user has to be told to go
// and read, which presumes they already know something is wrong. So failures are
// recorded here as well, and the tray tooltip shows the most recent one, which is
// the one surface that is always visible and costs no layout.
//
// Message-thread only. Everything that reports here already runs there.
//==============================================================================
namespace lighthost::status
{
    struct Problem
    {
        juce::Time   when;
        juce::String message;
    };

    class Sink
    {
    public:
        /** Records a failure and logs it. Callers should not log it themselves:
            one report, one line, one place that decides the wording.
        */
        void report (const juce::String& message)
        {
            juce::Logger::writeToLog ("Problem: " + message);

            problems.push_back ({ juce::Time::getCurrentTime(), message });

            // Bounded: this is a status surface, not a history. The log is the
            // history.
            if (problems.size() > kMaxRemembered)
                problems.erase (problems.begin());

            ++total;

            if (onChange != nullptr)
                onChange();
        }

        [[nodiscard]] bool hasProblem() const noexcept       { return ! problems.empty(); }
        [[nodiscard]] int  totalReported() const noexcept    { return total; }

        /** The most recent problem, or an empty string if there has been none. */
        [[nodiscard]] juce::String mostRecent() const
        {
            return problems.empty() ? juce::String() : problems.back().message;
        }

        [[nodiscard]] const std::vector<Problem>& all() const noexcept { return problems; }

        /** Forgets the problems without resetting the total, so a cleared sink
            still admits that something happened earlier.
        */
        void clear()
        {
            if (problems.empty())
                return;

            problems.clear();

            if (onChange != nullptr)
                onChange();
        }

        /** Called after any change, so a UI can refresh itself. */
        std::function<void()> onChange;

        static constexpr size_t kMaxRemembered = 8;

    private:
        std::vector<Problem> problems;
        int total = 0;
    };
}
