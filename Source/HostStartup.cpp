#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "IconMenu.hpp"

#if ! (JUCE_PLUGINHOST_VST3 || JUCE_PLUGINHOST_AU)
 #error "If you're building the audio plugin host, you probably want to enable VST3 and/or AU support"
#endif

class PluginHostApp final : public juce::JUCEApplication
{
public:
    PluginHostApp() = default;

    void initialise ([[maybe_unused]] const juce::String& commandLine) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = getApplicationName();
        options.filenameSuffix      = "settings";
        options.osxLibrarySubFolder = "Preferences";

        applyMultiInstanceSuffix (options);

        appProperties = std::make_unique<juce::ApplicationProperties>();
        appProperties->setStorageParameters (options);

        // Single rotating log file at <appData>/Light Host/LightHost.log.
        // JUCE's FileLogger ctor trims the existing file to maxInitialFileSizeBytes
        // (256 KB) on open — so the file size is bounded across an arbitrary
        // number of sessions. Welcome banner makes sessions visually separable
        // inside the rotated file.
        constexpr juce::int64 kMaxLogBytes = 256 * 1024;
        const auto logDir = juce::FileLogger::getSystemLogFileFolder()
                                .getChildFile (getApplicationName());
        logDir.createDirectory();

        const auto banner = "\n==== Light Host " + getApplicationVersion()
                          + " starting at "
                          + juce::Time::getCurrentTime().toString (true, true)
                          + " ====";

        fileLogger.reset (new juce::FileLogger (logDir.getChildFile ("LightHost.log"),
                                                banner,
                                                kMaxLogBytes));
        juce::Logger::setCurrentLogger (fileLogger.get());
        juce::Logger::writeToLog ("PluginHostApp: initialise");

        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        iconMenu = std::make_unique<IconMenu>();

        #if JUCE_MAC
            juce::Process::setDockIconVisible (false);
        #endif
    }

    void shutdown() override
    {
        juce::Logger::writeToLog ("PluginHostApp: shutdown");
        iconMenu.reset();
        appProperties.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        juce::Logger::setCurrentLogger (nullptr);
        fileLogger.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    const juce::String getApplicationName() override       { return "Light Host"; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }

    bool moreThanOneInstanceAllowed() override
    {
        return getMultiInstanceName().isNotEmpty();
    }

    std::unique_ptr<juce::ApplicationProperties> appProperties;
    std::unique_ptr<juce::FileLogger> fileLogger;
    juce::LookAndFeel_V4 lookAndFeel;

private:
    std::unique_ptr<IconMenu> iconMenu;

    [[nodiscard]] juce::String getMultiInstanceName() const
    {
        for (const auto& param : getCommandLineParameterArray())
        {
            if (param.startsWith ("-multi-instance="))
                return param.fromFirstOccurrenceOf ("=", false, false);
        }
        return {};
    }

    void applyMultiInstanceSuffix (juce::PropertiesFile::Options& options) const
    {
        if (auto instanceName = getMultiInstanceName(); instanceName.isNotEmpty())
            options.filenameSuffix = instanceName + "." + options.filenameSuffix;
    }
};

static PluginHostApp& getApp()                             { return *dynamic_cast<PluginHostApp*> (juce::JUCEApplication::getInstance()); }
juce::ApplicationProperties& getAppProperties()            { return *getApp().appProperties; }

START_JUCE_APPLICATION (PluginHostApp)
