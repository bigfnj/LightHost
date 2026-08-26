#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// The -multi-instance=NAME argument, made safe to put in a filename.
//
// The name is concatenated into the settings filename so a second instance keeps
// its own configuration. It arrives from the command line, and up to 5.0.0 it was
// used exactly as given.
//
// Two things follow from that. A name containing a path separator or "..'"
// escapes the settings folder, so `-multi-instance=../../something` writes
// wherever that resolves to. A name containing a character the filesystem rejects
// produces a file that cannot be opened, and since every settings write already
// ignored its result, the user's configuration would silently never be saved.
//
// So the name is reduced to characters that are safe everywhere, and bounded. A
// name that survives sanitising to nothing does not fall back to the default
// settings file, because that would put a deliberately separate instance back on
// top of the primary one; it gets a fixed name of its own instead.
//==============================================================================
namespace lighthost::instance
{
    /** Longest accepted name. Filenames have limits, and the name is only a label
        for a settings file, not a place to put a sentence.
    */
    static constexpr int kMaxNameLength = 32;

    /** Used when a name was given but nothing safe survived it. */
    inline const juce::String kFallbackName { "instance" };

    /** True for the characters allowed in an instance name. */
    [[nodiscard]] inline bool isAllowed (juce::juce_wchar character) noexcept
    {
        return juce::CharacterFunctions::isLetterOrDigit (character)
            || character == '-'
            || character == '_';
    }

    /** Reduces a name from the command line to something safe to put in a
        filename. Returns an empty string only when the input was empty, which
        means no second instance was asked for.
    */
    [[nodiscard]] inline juce::String sanitise (const juce::String& raw)
    {
        if (raw.isEmpty())
            return {};

        juce::String safe;

        for (auto character : raw)
        {
            if (safe.length() >= kMaxNameLength)
                break;

            if (isAllowed (character))
                safe += character;
        }

        return safe.isEmpty() ? kFallbackName : safe;
    }
}
