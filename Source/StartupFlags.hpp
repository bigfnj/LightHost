#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Command-line flags that modify an ORDINARY run, as opposed to the three that
// take the process over and exit.
//
// --render, --scan and -self-test each live with the module that implements
// them, and each exposes an isRequested the tests can reach. -preferences had
// no module to live in, so it was parsed inline in HostStartup::initialise --
// the one flag with no test and no home. Being inline is also how it came to
// accept only one of its two spellings while every other flag accepted both.
//==============================================================================
namespace lighthost::startup
{
    /** True when -preferences or --preferences appears on the command line.

        Both spellings, like every other flag in this application. The inline
        parse this replaces matched "-preferences" alone, so `--preferences`
        started the app with no window and no complaint -- and the double dash
        is the natural guess, because --render and --scan use it.

        The single dash remains the documented form. The split is not cosmetic:
        --render and --scan are MODES that take the process over and exit, while
        this is an ordinary run that happens to open a window, which is what
        -self-test and -multi-instance also are.
    */
    [[nodiscard]] inline bool openPreferencesRequested (const juce::StringArray& params)
    {
        return params.contains ("-preferences") || params.contains ("--preferences");
    }
}
