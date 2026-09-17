#include "../Source/PluginScan.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// Where a headless scan looks.
//
// WHY THIS FILE EXISTS
//
// PluginScan.hpp:75 says combineSearchPaths is "pure, so the precedence is
// testable without a plugin on disk". Nothing tested it. A claim in a comment
// that no test backs is the same shape as the problem this module was written
// to solve: tools/render-regression.sh used to depend on a person clicking Scan
// in a window a headless run does not have, so it looked like a gate and was a
// gate nobody could run.
//
// What a mistake here costs is quiet. Drop the defaults and --render --chain
// NAME stops resolving, so the audio-regression gate reports "no scanned plugin
// matches" and cannot run. Drop a --scan-path and the plugin under test is
// simply absent from the list, and the render falls back to whatever else
// matched the name. Neither says anything about a search path.
//
// The interesting cases are all on the input side -- a repeated flag, a flag at
// the end of the line with nothing after it, a quoted folder name, a directory
// that is not there -- because those are what a shell hands over and none of
// them is a wrong TYPE that the compiler could catch.
//==============================================================================
namespace
{

class PluginScanPathTests final : public juce::UnitTest
{
public:
    PluginScanPathTests()
        : juce::UnitTest ("Headless scan search paths", "PluginScan") {}

    void runTest() override
    {
        using namespace lighthost::scan;

        // Real directories, because combineSearchPaths asks the filesystem
        // whether each extra path IS one -- dropping the ones that are not is
        // the behaviour under test, and it cannot be exercised with strings.
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("LightHostScanTests-"
                                             + juce::String (juce::Random::getSystemRandom()
                                                                 .nextInt (1 << 30)));

        const auto vendor    = root.getChildFile ("Vendor");
        const auto common    = root.getChildFile ("Common VST3");
        const auto nested    = common.getChildFile ("Acme");
        const auto absent    = root.getChildFile ("NotOnDisk");
        const auto plainFile = root.getChildFile ("NotADirectory.vst3");

        // A sibling of the other two, deliberately not inside either, for the
        // ordering test below. Using a nested folder there would exercise the
        // redundancy collapse instead of the precedence it means to check.
        const auto commandLine = root.getChildFile ("CommandLine");

        vendor.createDirectory();
        common.createDirectory();
        nested.createDirectory();
        commandLine.createDirectory();
        plainFile.replaceWithText ("a file, not a folder");

        const auto quote = juce::String::charToString ('"');

        //======================================================================
        beginTest ("--scan is recognised in both spellings and nothing else is");
        {
            expect (isRequested ({ "--scan" }));
            expect (isRequested ({ "-scan" }));
            expect (isRequested ({ "-multi-instance=rig", "--scan" }));

            expect (! isRequested ({}));
            expect (! isRequested ({ "-self-test" }));
            expect (! isRequested ({ "scan" }), "a bare word is not the flag");

            // --scan-path without --scan is a path for a scan that was never
            // asked for. Worth pinning: the two flags look alike, and if this
            // matched, naming a folder would silently start a scan that loads
            // third-party code in process.
            expect (! isRequested ({ "--scan-path", vendor.getFullPathName() }));
        }

        //======================================================================
        beginTest ("--scan-path is repeatable, and the order is kept");
        {
            const auto paths = parseExtraPaths ({ "--scan-path", "/one",
                                                  "-scan-path",  "/two",
                                                  "--scan-path", "/three" });

            expectEquals (paths.size(), 3);
            expectEquals (paths.joinIntoString ("|"), juce::String ("/one|/two|/three"));
        }

        beginTest ("--scan-path at the end of the line contributes nothing");
        {
            // The flag with nothing after it. Reading past the end would be an
            // out-of-range juce::StringArray access, which returns an empty
            // string rather than faulting -- so the failure would be a scan
            // that silently searched one folder fewer than it printed.
            expectEquals (parseExtraPaths ({ "--scan-path" }).size(), 0);
            expectEquals (parseExtraPaths ({ "--scan", "-scan-path" }).size(), 0);
        }

        beginTest ("a quoted folder name loses its quotes");
        {
            juce::StringArray params;
            params.add ("--scan-path");
            params.add (quote + common.getFullPathName() + quote);

            const auto paths = parseExtraPaths (params);

            expectEquals (paths.size(), 1);
            expectEquals (paths[0], common.getFullPathName(),
                          "a quoted path kept its quotes, so no directory matches it");
        }

        beginTest ("whitespace is trimmed and a blank value is dropped");
        {
            juce::StringArray params;
            params.add ("--scan-path");
            params.add ("   " + vendor.getFullPathName() + "   ");
            params.add ("--scan-path");
            params.add ("    ");

            const auto paths = parseExtraPaths (params);

            expectEquals (paths.size(), 1,
                          "a whitespace-only value should not become a search path");
            expectEquals (paths[0], vendor.getFullPathName());
        }

        beginTest ("no --scan-path means no extra paths, not one empty one");
        {
            expect (parseExtraPaths ({}).isEmpty());
            expect (parseExtraPaths ({ "--scan" }).isEmpty());
        }

        //======================================================================
        beginTest ("the format's own defaults are kept, and come first");
        {
            // An addition, never a replacement. A --scan-path that replaced the
            // defaults would make one named folder the only place a plugin can
            // live, and every plugin already installed normally would vanish
            // from the user's list on the next scan.
            juce::FileSearchPath defaults;
            defaults.add (vendor);

            const auto combined = combineSearchPaths (defaults, {}, { common.getFullPathName() });

            expectEquals (combined.getNumPaths(), 2);
            expectEquals (combined[0].getFullPathName(), vendor.getFullPathName(),
                          "the defaults should still be searched, and searched first");
            expectEquals (combined[1].getFullPathName(), common.getFullPathName());
        }

        beginTest ("a remembered path sits between the defaults and the command line");
        {
            // Three sources, one order: what the format knows, what the user
            // added in the Available Plugins window, then what this run named.
            juce::FileSearchPath defaults;
            defaults.add (vendor);

            juce::FileSearchPath remembered;
            remembered.add (common);

            const auto combined = combineSearchPaths (defaults, remembered,
                                                      { commandLine.getFullPathName() });

            expectEquals (combined.getNumPaths(), 3);
            expectEquals (combined[0].getFullPathName(), vendor.getFullPathName());
            expectEquals (combined[1].getFullPathName(), common.getFullPathName());
            expectEquals (combined[2].getFullPathName(), commandLine.getFullPathName());
        }

        beginTest ("a path already in the defaults is not added twice");
        {
            // The scan is recursive, so a duplicated folder is every plugin
            // under it opened twice -- minutes of extra work on a real machine,
            // and two chances for one crashy plugin to trip the dead man's
            // pedal.
            juce::FileSearchPath defaults;
            defaults.add (vendor);
            defaults.add (common);

            juce::FileSearchPath remembered;
            remembered.add (common);

            const auto combined = combineSearchPaths (defaults, remembered,
                                                      { vendor.getFullPathName(),
                                                        common.getFullPathName() });

            expectEquals (combined.getNumPaths(), 2,
                          "the same folder arrived from all three sources and should "
                          "appear once");
        }

        beginTest ("a directory that is not there is skipped");
        {
            // A typo, or a drive that is not mounted. juce::PluginDirectoryScanner
            // is given the whole path at once, so an entry that exists nowhere
            // is at best wasted and at worst a stall on a dead network share.
            juce::FileSearchPath defaults;
            defaults.add (vendor);

            const auto combined = combineSearchPaths (defaults, {},
                                                      { absent.getFullPathName() });

            expect (! absent.exists(), "precondition: this path must not exist");
            expectEquals (combined.getNumPaths(), 1);
            expectEquals (combined[0].getFullPathName(), vendor.getFullPathName());
        }

        beginTest ("a file that is not a directory is skipped too");
        {
            // Dragging a plugin onto a terminal gives you the plugin, not its
            // folder. isDirectory() is false for a file, so it drops out.
            juce::FileSearchPath defaults;
            defaults.add (vendor);

            const auto combined = combineSearchPaths (defaults, {},
                                                      { plainFile.getFullPathName() });

            expect (plainFile.existsAsFile(), "precondition: this must be a file");
            expectEquals (combined.getNumPaths(), 1,
                          "a file was accepted as a folder to search");
        }

        beginTest ("an empty everything produces an empty path, not a crash");
        {
            // run() skips a format whose combined path is empty rather than
            // handing juce::PluginDirectoryScanner nothing to look at.
            const juce::FileSearchPath nothing;

            expectEquals (combineSearchPaths (nothing, nothing, {}).getNumPaths(), 0);
        }

        beginTest ("a --scan-path inside a default location is added alongside it");
        {
            // CURRENT BEHAVIOUR, pinned rather than endorsed.
            //
            // addIfNotAlreadyThere compares whole paths, so a folder INSIDE a
            // default location is not a duplicate of it and used to go in as a
            // second entry. The scan is recursive, so everything under the
            // nested folder was then scanned twice -- once reached from the
            // parent and once from the entry itself -- and scanning loads each
            // plugin, so it is not free.
            //
            // combineSearchPaths now ends with removeRedundantPaths(), which
            // is JUCE's version of the isFileInPath guard
            // IconMenu::rememberSearchPath applies by hand.
            juce::FileSearchPath defaults;
            defaults.add (common);

            const auto combined = combineSearchPaths (defaults, {}, { nested.getFullPathName() });

            expectEquals (combined.getNumPaths(), 1,
                          "a path inside a default location is redundant and should be collapsed");
            expect (combined.isFileInPath (nested.getChildFile ("Acme EQ.vst3"), true),
                    "collapsing must not lose coverage of the nested folder");

            // The parent entry ALONE already reaches everything the nested
            // entry would, which is what makes the second entry redundant work
            // rather than extra coverage.
            expect (defaults.isFileInPath (nested.getChildFile ("Acme EQ.vst3"), true),
                    "the parent entry should already cover the nested folder");
        }

        root.deleteRecursively();
    }
};

PluginScanPathTests pluginScanPathTests;

} // namespace
