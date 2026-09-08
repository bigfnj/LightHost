#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "IconMenu.hpp"
#include "InstanceName.hpp"
#include "LookAndFeel.hpp"
#include "OfflineRender.hpp"
#include "SelfTest.hpp"

#if ! (JUCE_PLUGINHOST_VST3 || JUCE_PLUGINHOST_AU)
 #error "If you're building the audio plugin host, you probably want to enable VST3 and/or AU support"
#endif

class PluginHostApp final : public juce::JUCEApplication
{
public:
    PluginHostApp() = default;

    void initialise ([[maybe_unused]] const juce::String& commandLine) override
    {
        selfTest    = lighthost::selftest::isRequested (getCommandLineParameterArray());
        renderMode  = lighthost::render::isRequested (getCommandLineParameterArray());

        juce::PropertiesFile::Options options;
        options.applicationName     = getApplicationName();
        options.filenameSuffix      = "settings";
        options.osxLibrarySubFolder = "Preferences";

        applyMultiInstanceSuffix (options);

        // A self-test must never read or overwrite the user's real configuration,
        // so it gets a settings folder unique to the run. folderName is honoured
        // on all three platforms, unlike environment redirection: on Windows the
        // settings root comes from CSIDL_APPDATA, which does not reliably follow
        // %APPDATA%.
        if (selfTest)
            options.folderName = lighthost::selftest::makeRunFolderName();

        appProperties = std::make_unique<juce::ApplicationProperties>();
        appProperties->setStorageParameters (options);

        // Single rotating log file at <appData>/Light Host/LightHost.log.
        // JUCE's FileLogger ctor trims the existing file to maxInitialFileSizeBytes
        // (256 KB) on open — so the file size is bounded across an arbitrary
        // number of sessions. Welcome banner makes sessions visually separable
        // inside the rotated file.
        constexpr juce::int64 kMaxLogBytes = 256 * 1024;

        // In self-test mode the log lives beside the throwaway settings file, so a
        // single folder holds everything the run produced.
        const auto logDir = selfTest
                                ? appProperties->getUserSettings()->getFile().getParentDirectory()
                                : juce::FileLogger::getSystemLogFileFolder()
                                      .getChildFile (getApplicationName());
        logDir.createDirectory();

        const auto banner = "\n==== Light Host " + getApplicationVersion()
                          + " starting at "
                          + juce::Time::getCurrentTime().toString (true, true)
                          + " ====";

        logFile = logDir.getChildFile ("LightHost.log");

        fileLogger.reset (new juce::FileLogger (logFile, banner, kMaxLogBytes));
        juce::Logger::setCurrentLogger (fileLogger.get());
        juce::Logger::writeToLog ("PluginHostApp: initialise");

        if (instanceNameWarning.isNotEmpty())
            juce::Logger::writeToLog (instanceNameWarning);

        // A render is not an application run: no audio device, no tray icon, no
        // Preferences, and above all nothing written back. It reads the chain the
        // user configured, pushes a file through it, and quits.
        if (renderMode)
        {
            runRender();
            return;
        }

        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        // Seeded before IconMenu is constructed, so the run starts from settings
        // written by an older version rather than from nothing. This is what makes
        // the self-test cover the one-shot chain-settings migration.
        if (selfTest)
            lighthost::selftest::seedLegacyChain (*appProperties->getUserSettings());

        iconMenu = std::make_unique<IconMenu>();

        #if JUCE_MAC
            juce::Process::setDockIconVisible (false);
        #endif

        if (selfTest)
            scheduleSelfTestCheck();
    }

    void shutdown() override
    {
        juce::Logger::writeToLog ("PluginHostApp: shutdown");
        iconMenu.reset();

        const auto settingsFile = appProperties != nullptr
                                      ? appProperties->getUserSettings()->getFile()
                                      : juce::File();

        appProperties.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        juce::Logger::setCurrentLogger (nullptr);
        fileLogger.reset();   // closes the log so it can be read back

        if (selfTest)
            finishSelfTest (settingsFile);
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    const juce::String getApplicationName() override       { return "Light Host"; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }

    bool moreThanOneInstanceAllowed() override
    {
        // A second instance normally hands off to the running one and quits. In
        // self-test mode that would exit 0 having done nothing, which is a false
        // pass — the worst kind of test result — so a self-test always runs.
        // A render must never hand off either. Handing off would exit 0 having
        // rendered nothing, which reads as success -- the same false pass the
        // self-test guards against.
        return lighthost::selftest::isRequested (getCommandLineParameterArray())
            || lighthost::render::isRequested (getCommandLineParameterArray())
            || getMultiInstanceName().isNotEmpty();
    }

    /** A second launch hands its command line here, and then that process exits.

        This used to be unhandled, so the second process died silently: nothing
        in the log, nothing on screen. Double-click a freshly built copy while an
        older one is already running and the visible result is *nothing at all*,
        which leaves the user believing they are running the build they just
        launched when they are still running the old one. Every subsequent
        observation is then made against the wrong process.

        So say it, on the two surfaces that are always there: the log, and the
        status row that the tray tooltip and Preferences both read.
    */
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        const auto arguments = commandLine.trim().isEmpty() ? juce::String ("no arguments")
                                                           : commandLine.trim();

        juce::Logger::writeToLog ("PluginHostApp: another instance was launched ("
                                  + arguments + "). This instance is already running, so the "
                                  "new process has exited without starting. Nothing has "
                                  "changed, including which executable is running.");

        if (iconMenu != nullptr)
        {
            iconMenu->reportStatus ("Light Host was already running - the copy you just "
                                    "launched has exited. To run a different build, quit "
                                    "from the tray first, or pass -multi-instance=NAME.");
            iconMenu->showPreferencesWindow();
        }
    }

    std::unique_ptr<juce::ApplicationProperties> appProperties;
    std::unique_ptr<juce::FileLogger> fileLogger;
    lighthost::ui::LookAndFeel lookAndFeel;

    [[nodiscard]] juce::File getCurrentLogFile() const { return logFile; }

private:
    std::unique_ptr<IconMenu> iconMenu;

    bool selfTest   = false;
    bool renderMode = false;
    juce::File logFile;
    juce::StringArray selfTestFailures;
    juce::String instanceNameWarning;

    [[nodiscard]] juce::String getMultiInstanceName() const
    {
        for (const auto& param : getCommandLineParameterArray())
        {
            if (param.startsWith ("-multi-instance="))
                return param.fromFirstOccurrenceOf ("=", false, false);
        }
        return {};
    }

    void applyMultiInstanceSuffix (juce::PropertiesFile::Options& options)
    {
        const auto requested = getMultiInstanceName();

        if (requested.isEmpty())
            return;

        // Sanitised before it reaches a filename: see Source/InstanceName.hpp for
        // what an unsanitised one could do.
        const auto safe = lighthost::instance::sanitise (requested);

        // Held rather than logged: this runs before the file logger exists, so a
        // message written here would go nowhere.
        if (safe != requested)
            instanceNameWarning = "PluginHostApp: instance name '" + requested
                                + "' is not usable in a filename; using '" + safe + "'";

        options.filenameSuffix = safe + "." + options.filenameSuffix;
    }

    /** Lets initialise() return so the real message loop runs, then checks and
        quits. Calling quit() from inside initialise() would skip the dispatch
        loop entirely, leaving the timer, the change listeners and the async
        plugin-load marshalling untested.
    */
    /** Renders a file through the configured chain and quits.

        Reports to stdout as well as the log, because this is run from a shell and
        a result that only lands in a log file is a result nobody reads.
    */
    void runRender()
    {
        juce::String inPath, outPath;
        lighthost::render::parseArguments (getCommandLineParameterArray(), inPath, outPath);

        if (inPath.isEmpty() || outPath.isEmpty())
        {
            std::printf ("RENDER FAIL: usage: \"Light Host\" --render <input.wav> <output.wav>\n");
            std::fflush (stdout);
            setApplicationReturnValue (2);
            quit();
            return;
        }

        const juce::File input (juce::File::getCurrentWorkingDirectory().getChildFile (inPath));
        const juce::File output (juce::File::getCurrentWorkingDirectory().getChildFile (outPath));

        auto* settings = appProperties->getUserSettings();
        const auto settingsFile = settings->getFile();
        const auto stateDir = settingsFile.getSiblingFile (
            settingsFile.getFileNameWithoutExtension() + ".state");

        juce::Logger::writeToLog ("Render: " + input.getFullPathName()
                                  + " -> " + output.getFullPathName());

        const auto overrides = lighthost::render::parseOverrides (juce::JUCEApplication::getCommandLineParameterArray());

        const auto paced = lighthost::render::isPaced (
            juce::JUCEApplication::getCommandLineParameterArray());

        std::printf ("  pacing           : %s\n",
                     paced ? "real time"
                           : "none (--fast; wrong for worker-thread plugins)");

        const auto chainOverride = lighthost::render::parseChain (
            juce::JUCEApplication::getCommandLineParameterArray());

        if (! chainOverride.isEmpty())
            std::printf ("  chain override   : %s\n",
                         chainOverride.joinIntoString (" -> ").toRawUTF8());

        const auto result = lighthost::render::renderFile (*settings, stateDir,
                                                           input, output, 480,
                                                           overrides, paced,
                                                           chainOverride);

        if (result.ok)
        {
            std::printf ("RENDER OK\n");
            std::printf ("  input            : %s\n", input.getFullPathName().toRawUTF8());
            std::printf ("  output           : %s\n", output.getFullPathName().toRawUTF8());
            std::printf ("  sample rate      : %.0f Hz\n", result.sampleRate);
            std::printf ("  plugins active   : %d\n", result.pluginsLoaded);
            std::printf ("  plugins bypassed : %d\n", result.pluginsBypassed);
            std::printf ("  plugins failed   : %d\n", result.pluginsFailed);
            std::printf ("  connections      : %d\n", result.connections);
            std::printf ("  declared latency : %d samples (%.2f ms)\n",
                         result.declaredLatency,
                         result.sampleRate > 0.0
                             ? result.declaredLatency * 1000.0 / result.sampleRate : 0.0);
            std::printf ("  frames written   : %lld\n", (long long) result.framesWritten);

            std::printf ("  realtime factor  : %.3f%s\n",
                         result.realtimeFactor,
                         // blocksTotal only counts up when pacing, so zero means
                         // --fast. Printing "(kept pace)" there would be a lie of
                         // exactly the kind this whole check exists to catch.
                         ! result.wasPaced
                             ? "  (unpaced - worker-thread plugins will misreport)"
                             : (result.blocksBehind > 0 ? "" : "  (kept pace)"));

            if (result.blocksBehind > 0)
                std::printf ("  NOTE             : render fell behind real time on %d of %d "
                             "blocks. Harmless for a plugin that does its work inline; "
                             "if it runs inference on a worker thread, it was starved for "
                             "that long and the result understates it.\n",
                             result.blocksBehind, result.blocksTotal);

            if (! result.parameterReport.isEmpty())
            {
                std::printf ("  active plugin parameters (after state restore):\n");
                for (const auto& line : result.parameterReport)
                    std::printf ("    %s\n", line.toRawUTF8());
            }
        }
        else
        {
            std::printf ("RENDER FAIL: %s\n", result.message.toRawUTF8());
        }

        std::fflush (stdout);
        juce::Logger::writeToLog ("Render: " + result.message);
        setApplicationReturnValue (result.ok ? 0 : 1);
        quit();
    }

    void scheduleSelfTestCheck()
    {
        constexpr int kDwellMs   = 1500;
        constexpr int kOpenUIMs  = 700;

        // Build the Preferences window part way through the run. It is the only
        // window this application has and it holds every control, so constructing
        // and tearing it down is worth exercising; nothing else in an automated
        // run ever does. Whether it *looks* right is still a manual check.
        juce::Timer::callAfterDelay (kOpenUIMs, [this]
        {
            if (iconMenu != nullptr)
                iconMenu->showPreferencesWindow();
        });

        // Wait for the condition, not for a duration.
        //
        // This used to be a flat 1500 ms dwell and then read the log. That is
        // ample on a developer machine and not always enough on a loaded CI
        // runner: the macOS release job failed exactly the two assertions that
        // depend on the asynchronous plugin load having finished, while the CI
        // workflow passed on the very same commit. A release gate that fails a
        // coin toss teaches people to re-run it, which is how a real failure
        // gets waved through.
        //
        // Polling for the completion marker is deterministic and usually faster
        // than the old fixed wait. Giving up is still a failure, and says so.
        juce::Timer::callAfterDelay (kDwellMs, [this] { awaitLoadComplete (0); });
    }

    void awaitLoadComplete (int waitedMs)
    {
        constexpr int kStepMs   = 100;
        constexpr int kGiveUpMs = 20000;

        const bool finished =
            logFile.existsAsFile()
            && logFile.loadFileAsString().contains ("loadActivePlugins complete");

        if (! finished && waitedMs < kGiveUpMs)
        {
            juce::Timer::callAfterDelay (kStepMs, [this, waitedMs]
            {
                awaitLoadComplete (waitedMs + kStepMs);
            });
            return;
        }

        if (! finished)
            juce::Logger::writeToLog ("SelfTest: gave up waiting for the plugin load after "
                                      + juce::String (kGiveUpMs) + " ms");

        auto* settings = appProperties->getUserSettings();

        selfTestFailures.addArray (
            lighthost::selftest::checkAfterStartup (logFile, settings->getFile()));

        selfTestFailures.addArray (
            lighthost::selftest::checkSeededChainMigrated (*settings));

        quit();
    }

    void finishSelfTest (const juce::File& settingsFile)
    {
        selfTestFailures.addArray (lighthost::selftest::checkAfterShutdown (logFile));
        selfTestFailures.addArray (lighthost::selftest::checkSeededStateSurvived (settingsFile));

        setApplicationReturnValue (
            lighthost::selftest::report (selfTestFailures, settingsFile.getParentDirectory()));
    }
};

static PluginHostApp& getApp()                             { return *dynamic_cast<PluginHostApp*> (juce::JUCEApplication::getInstance()); }
juce::ApplicationProperties& getAppProperties()            { return *getApp().appProperties; }

/** Where this run is logging. Follows the self-test's throwaway folder, so a
    self-test never points the user at the real log.
*/
juce::File getLogFile()                                    { return getApp().getCurrentLogFile(); }

START_JUCE_APPLICATION (PluginHostApp)
