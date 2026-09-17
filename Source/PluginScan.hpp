#pragma once

#include "SettingsKeys.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// Scanning for plugins without a user interface.
//
// WHY THIS EXISTS
//
// Everything this project uses to prove it has not broken the audio runs
// through `--render`, and `--render --chain NAME` resolves NAME against the
// scanned plugin list in the settings file. On a machine that has never opened
// the Available Plugins window that list is empty, so the render gate reports
// "no scanned plugin matches" and the one check that can prove a sample did not
// change cannot be run at all.
//
// Until now the only way to populate it was a person clicking Scan in a window
// that a headless run does not have. tools/render-regression.sh therefore
// depended on manual setup it could not perform or verify, which is the same
// class of problem as a gate that reports success it has not earned: it looks
// like a check and it is a check nobody can run.
//
// This does what the Available Plugins window's scan button does, from the
// command line, and writes the same settings key. It is a setup step, not a
// test -- it loads third-party code in-process by design, so it is deliberately
// not wired into ctest.
//
// The dead man's pedal is the same file the window uses, so a plugin that
// crashes a scan here is skipped by the window afterwards and the other way
// round.
//==============================================================================
namespace lighthost::scan
{
    struct Result
    {
        bool              ok = false;
        int               listed = 0;       // plugins in the list when finished
        int               added = 0;        // how many of those were new
        juce::StringArray failedFiles;      // looked like plugins, would not load
        juce::StringArray formatsScanned;
        juce::String      message;
    };

    /** True when --scan appears on the command line. */
    [[nodiscard]] inline bool isRequested (const juce::StringArray& params)
    {
        return params.contains ("--scan") || params.contains ("-scan");
    }

    /** Extra folders from `--scan-path DIR` (repeatable).

        Separate from the format's default locations rather than replacing them:
        a plugin in an unusual folder is an addition to the standard set, never a
        reason to stop looking in the standard set.
    */
    [[nodiscard]] inline juce::StringArray parseExtraPaths (const juce::StringArray& params)
    {
        juce::StringArray out;

        for (int i = 0; i < params.size(); ++i)
            if ((params[i] == "--scan-path" || params[i] == "-scan-path") && i + 1 < params.size())
                out.add (params[i + 1].unquoted());

        out.trim();
        out.removeEmptyStrings();
        return out;
    }

    /** Where one format should look: its own defaults, whatever the user has
        added through the Available Plugins window, and anything named on the
        command line.

        Pure, so the precedence is testable without a plugin on disk.
    */
    [[nodiscard]] inline juce::FileSearchPath combineSearchPaths (const juce::FileSearchPath& defaults,
                                                                  const juce::FileSearchPath& remembered,
                                                                  const juce::StringArray& extra)
    {
        juce::FileSearchPath combined = defaults;

        for (int i = 0; i < remembered.getNumPaths(); ++i)
            combined.addIfNotAlreadyThere (remembered[i]);

        for (const auto& dir : extra)
        {
            const juce::File file (dir);

            if (file.isDirectory())
                combined.addIfNotAlreadyThere (file);
        }

        return combined;
    }

    //==========================================================================
    /** Scans every format that can be scanned and writes the result to the
        settings file under the same key the application reads at startup.

        `onProgress` is called with each file as it is tried, so a caller can
        show something during a scan that takes minutes. It may be null.
    */
    inline Result run (juce::PropertiesFile& settings,
                       const juce::StringArray& extraPaths,
                       const std::function<void (const juce::String&)>& onProgress)
    {
        Result result;

        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);

        juce::KnownPluginList known;

        // Start from what is already known rather than from nothing, so a rescan
        // keeps plugins whose folder is no longer in the search path and updates
        // the ones it does find in place. Clearing first would make every scan a
        // destructive operation on the user's list.
        if (auto xml = settings.getXmlValue (keys::pluginList))
            known.recreateFromXml (*xml);

        const auto before = known.getNumTypes();

        const juce::File deadMansPedal (settings.getFile()
                                            .getSiblingFile ("RecentlyCrashedPluginsList"));

        for (int i = 0; i < formats.getNumFormats(); ++i)
        {
            auto* format = formats.getFormat (i);

            if (format == nullptr || ! format->canScanForPlugins())
                continue;

            const auto path = combineSearchPaths (
                format->getDefaultLocationsToSearch(),
                juce::PluginListComponent::getLastSearchPath (settings, *format),
                extraPaths);

            if (path.getNumPaths() == 0)
                continue;

            result.formatsScanned.add (format->getName());

            // true = recursive, which is what the window's own scan does. A
            // vendor folder one level down is found from its parent.
            juce::PluginDirectoryScanner scanner (known, *format, path, true, deadMansPedal);

            juce::String next;

            while (scanner.scanNextFile (true, next))
                if (onProgress != nullptr)
                    onProgress (next);

            // Kept rather than discarded: a folder in which every plugin failed
            // to load has to look different from a folder that scanned cleanly.
            result.failedFiles.addArray (scanner.getFailedFiles());
        }

        result.listed = known.getNumTypes();
        result.added  = result.listed - before;

        if (result.formatsScanned.isEmpty())
        {
            result.message = "no plugin format had anywhere to look";
            return result;
        }

        if (auto xml = known.createXml())
        {
            settings.setValue (keys::pluginList, xml.get());

            // Written now rather than at shutdown. A scan is the whole point of
            // the run, and PropertiesFile only auto-saves on a timer -- so a
            // process that exits promptly after this can otherwise do all the
            // work and persist none of it.
            if (! settings.saveIfNeeded())
            {
                result.message = "scanned " + juce::String (result.listed)
                               + " plugin(s) but could not write "
                               + settings.getFile().getFullPathName();
                return result;
            }
        }

        result.ok = true;
        result.message = "scanned " + juce::String (result.listed) + " plugin(s)";
        return result;
    }
}
