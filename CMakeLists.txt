# 3.28 is the project's real floor (CMakePresets v6; JUCE itself asks for 3.22).
# Only the "Visual Studio 18 2026" generator needs 4.2, so require it there rather
# than locking out every Ninja/Clang/MinGW contributor. Ubuntu 24.04 packages
# CMake 3.28.3, which configures this project fine with Ninja.
cmake_minimum_required(VERSION 3.28)

if(CMAKE_GENERATOR MATCHES "Visual Studio 18 2026" AND CMAKE_VERSION VERSION_LESS 4.2)
    message(FATAL_ERROR
        "The Visual Studio 18 2026 generator requires CMake 4.2 or newer, "
        "but this is CMake ${CMAKE_VERSION}. Either upgrade CMake or use one of "
        "the Ninja presets.")
endif()

# ── Windows SDK version (Windows 11 26100) ──────────────────────────────────────
# Must be set before project() so the VS generator picks it up for WindowsTargetPlatformVersion.
set(CMAKE_SYSTEM_VERSION "10.0.26100.0" CACHE STRING "Windows SDK version" FORCE)

# ── macOS architectures and floor ───────────────────────────────────────────────
# Both must be set before project(): they are consumed while the compiler is being
# identified.
#
# Universal, so one download runs on Apple Silicon and on Intel. Without this the
# build produces a binary for whatever machine happened to compile it, and GitHub's
# macOS runners are Apple Silicon, so the published app would have refused to start
# for every Intel Mac owner.
#
# 11.0 is the floor because arm64 macOS starts there, so a universal binary cannot
# usefully target anything older.
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures to build")
endif()

if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum supported macOS version")
endif()

project(LightHost
    VERSION 5.2.0
    DESCRIPTION "Lightweight VST/AU/VST3 Audio Plugin Host"
    HOMEPAGE_URL "https://github.com/BigFNj"
    LANGUAGES C CXX
)

# ── C++23 standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Windows runtime library ─────────────────────────────────────────────────────
# Statically linked, so the shipped executable needs no Visual C++ redistributable.
# Light Host is distributed as a single .exe in a zip, and an installer whose only
# job would be to place one file is not worth having; a redistributable the user
# has to find first is worse.
#
# Set before add_subdirectory so JUCE's targets are created with it too. Mixing
# runtimes across targets that link together is a linker error at best.
#
# Plugins load with whatever runtime they were built against, which is normal for a
# host: each module owns its own CRT state, and nothing allocated on one side is
# freed on the other.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── JUCE ────────────────────────────────────────────────────────────────────────
# Expects JUCE 9.0.x at lib/juce (or override via -DJUCE_DIR=<path>)
if(NOT DEFINED JUCE_DIR)
    set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/juce")
endif()

add_subdirectory(${JUCE_DIR} juce_build EXCLUDE_FROM_ALL)

# ── Application target ──────────────────────────────────────────────────────────
juce_add_gui_app(LightHost
    PRODUCT_NAME        "Light Host"
    VERSION             "${PROJECT_VERSION}"
    COMPANY_NAME        "BigFN'j"
    COMPANY_WEBSITE     "https://github.com/BigFNj"
    COMPANY_EMAIL       ""
    BUNDLE_ID           "com.bigfnj.lighthost"
    ICON_BIG            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    ICON_SMALL          "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    NEEDS_CURL          FALSE
    NEEDS_WEB_BROWSER   FALSE
)

# ── Source files ────────────────────────────────────────────────────────────────
target_sources(LightHost PRIVATE
    Source/HostStartup.cpp
    Source/LookAndFeel.hpp
    Source/Lanes.hpp
    Source/NodeIds.hpp
    Source/ConfirmPolicy.hpp
    Source/HostServices.hpp
    Source/DevicePolicy.hpp
    Source/PluginScan.hpp
    Source/SettingsKeys.hpp
    Source/OfflineRender.hpp
    Source/PluginStateVault.hpp
    Source/DeviceTap.hpp
    Source/SignalMetering.hpp
    Source/SelfTest.hpp
    Source/UiMetrics.hpp
    Source/IconMenu.cpp
    Source/PluginWindow.cpp
    Source/PreferencesWindow.cpp
)

# ── Binary resources ────────────────────────────────────────────────────────────
juce_add_binary_data(LightHostData SOURCES
    Resources/TrayIcon.png
    Resources/TrayIconTemplate.png
)

target_link_libraries(LightHost PRIVATE LightHostData)

# ── Preprocessor definitions ────────────────────────────────────────────────────
target_compile_definitions(LightHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_CDBURNER=0
    JUCE_USE_CDREADER=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ALSA=1
    JUCE_ASIO=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LightHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LightHost,JUCE_VERSION>"

    # Route jassert failures into the log file, in release builds too.
    # Without this they are invisible outside a debugger: jassertfalse only breaks
    # when one is attached, so on CI an assertion would pass silently. Also means
    # field reports carry assertion sites in LightHost.log.
    JUCE_LOG_ASSERTIONS=1
)

# ── VST2 SDK include path ───────────────────────────────────────────────────────
# SYSTEM, so the compiler stops diagnosing code we are not allowed to change.
# These are Steinberg's 2006 headers under their own licence: aeffect.h uses
# strncpy and strncat and a __int64 extension token, which Clang warns about
# three times each. Left as ordinary includes they would be the only thing
# standing between this project and -Werror, and the fix would have to be
# either suppressing those warnings everywhere or editing a vendored SDK.
target_include_directories(LightHost SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/vstsdk2.4"
)

# ── Link JUCE modules ──────────────────────────────────────────────────────────
target_link_libraries(LightHost PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ── Platform-specific settings ──────────────────────────────────────────────────
if(WIN32)
    # Target Windows 11 22H2+ (build 10.0.22621.0)
    #
    # WINVER and _WIN32_WINNT were here too, at 0x0A00, and were redundant:
    # juce_BasicNativeHeaders.h defines both to _WIN32_WINNT_WIN10 -- the same
    # value -- unconditionally, before it includes windows.h. Ours arrived
    # first from the command line, so every translation unit redefined them.
    # MSVC says nothing about that; Clang calls it -Wmacro-redefined, which is
    # how it surfaced the moment warnings became errors.
    #
    # Relying on JUCE is safe here rather than merely tidy: the one place that
    # includes <windows.h> directly, IconMenu.cpp, does so after its JUCE
    # includes, so the macros are already set by then.
    #
    # NTDDI_VERSION stays, because JUCE does not set it, and it is the one of
    # the three that actually pins 22H2 rather than "the Windows 10 family".
    target_compile_definitions(LightHost PRIVATE
        NTDDI_VERSION=0x0A00000C
    )

    # Enable latest MSVC features.
    #
    # Guarded on MSVC, not on WIN32. These are slash-form flags that only the
    # MSVC driver understands, and clang++ on Windows is WIN32 too -- it takes
    # GNU-form flags and rejects "/utf-8" as a missing input file. So the
    # clang-release preset had never once built here: it died on the first
    # translation unit, and BACKLOG.md was recommending it as the way to catch
    # MSVC-only divergence before a push. CMake sets MSVC for cl and for
    # clang-cl, which are exactly the drivers that accept these.
    if(MSVC)
        target_compile_options(LightHost PRIVATE
            /W4             # Warning level 4
            /permissive-    # Standards conformance
            /Zc:__cplusplus # Report correct __cplusplus value
            /Zc:preprocessor # Conforming preprocessor
            /utf-8          # Source and execution charset UTF-8
        )
    endif()

    # An enumerator a switch does not handle, at warning level 1.
    #
    # Not on by default at ANY level: C4062 is off unless asked for by name, so
    # MSVC was the one compiler here that said nothing about a missing case.
    # GCC and Clang have warned all along, because JUCE's recommended flags
    # already pass -Wswitch-enum -- which means the silent-enumerator failure
    # PluginWindow.h's comment describes could only ever have happened on the
    # machine this project is developed on, and would have been caught by CI.
    # That is the wrong way round: catch it where it is written.
    #
    # /w14062 rather than /w14061, deliberately. 4061 fires for an enumerator
    # covered only by a default label, which is a legitimate pattern; 4062
    # fires only when there is no default AND no case, which is the one this
    # codebase relies on for its derived settings keys.
    if(MSVC)
        target_compile_options(LightHost PRIVATE /w14062)
    endif()

    # Link Windows 11 APIs
    target_link_libraries(LightHost PRIVATE
        dwmapi         # Desktop Window Manager (DWM) for Win11 visuals
    )
endif()

if(MSVC)
    # VS2026 / MSVC optimisations for Release builds
    target_compile_options(LightHost PRIVATE
        $<$<CONFIG:Release>:/O2 /GL /Gy>
    )
    target_link_options(LightHost PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )
endif()

# ── Warnings are errors ─────────────────────────────────────────────────────────
# Defined here and applied to both of our targets below, never to JUCE or to the
# VST2 SDK -- those are vendored, and failing a build on a diagnostic in code we
# are not going to edit would only teach someone to switch this off.
#
# WHY. /W4 and JUCE's warning flags were already on across three platforms, so
# every one of these was already being printed; nothing read them. Turning them
# into errors is what makes the compiler a gate rather than a commentator, and
# the class of thing it catches is not hypothetical here: 5.2.0 shipped a
# release-blocking break because MSVC resolved an operator<< overload that GCC
# and Clang refused, and that arrived from CI after the push.
#
# WHAT IT COST. Nine diagnostics, all real and all small: three from GCC (a
# constructor parameter shadowing AudioProcessor's own latencySamples, twice,
# and processBlockBypassed hiding its double overload) and six from Clang (two
# free functions with no declaration in scope, three shadowed names, and an
# int-to-size_t-and-back round trip on a juce::Array index). None was a bug that
# had bitten. One -- getLogFile declared separately in the file that used it --
# was a signature nothing was checking.
#
# WHEN THIS BITES YOU. A new compiler version introduces a warning and the build
# stops. Fix the warning. Do not reach for the switch: a compiler that is
# allowed to be ignored is not a gate, and the warnings had been accumulating
# unread for exactly that reason.
set(LIGHTHOST_WARNINGS_AS_ERRORS $<IF:$<BOOL:${MSVC}>,/WX,-Werror>)

# ── Unit tests ──────────────────────────────────────────────────────────────────
# Console app driven by juce::UnitTestRunner — no third-party test framework, as
# JUCE is already vendored. Build and run with:
#   cmake --build --preset release --target LightHostTests
#   ctest --test-dir build/release -C Release --output-on-failure
option(LIGHTHOST_BUILD_TESTS "Build the LightHost unit tests" ON)

if(LIGHTHOST_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(LightHostTests
        PRODUCT_NAME "LightHostTests"
        NEEDS_CURL        FALSE
        NEEDS_WEB_BROWSER FALSE
    )

    target_sources(LightHostTests PRIVATE
        Source/PluginWindow.cpp
        Tests/TestMain.cpp
        Tests/ConfirmPolicyTests.cpp
        Tests/DevicePolicyTests.cpp
        Tests/GainProcessorTests.cpp
        Tests/GraphRenderTests.cpp
        Tests/GraphTopologyTests.cpp
        Tests/InstanceNameTests.cpp
        Tests/NodeIdTests.cpp
        Tests/PluginChainStoreTests.cpp
        Tests/PluginStateTests.cpp
        Tests/PluginStateVaultTests.cpp
        Tests/PluginWindowTests.cpp
        Tests/SampleRatePolicyTests.cpp
        Tests/SettingsKeysTests.cpp
        Tests/SignalMeteringTests.cpp
        Tests/StatusSinkTests.cpp
        Tests/StubProcessors.hpp
    )

    target_compile_definitions(LightHostTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_FLAC=0
        JUCE_USE_OGGVORBIS=0
        JUCE_USE_CDBURNER=0
        JUCE_USE_CDREADER=0
        # Deliberately NOT defining JUCE_UNIT_TESTS=1: that compiles in JUCE's own
        # internal test suite (~60M assertions, and its failures are not ours).
        # juce::UnitTest itself is always available.
        JUCE_APPLICATION_NAME_STRING="LightHostTests"
        JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"

        # PluginWindow::closeAllCurrentlyOpenWindows pumps the message queue
        # inside a JUCE_MODAL_LOOPS_PERMITTED block, and the default is 0. Without
        # this, the tests would compile that block out and assert against a
        # function the application never runs.
        JUCE_MODAL_LOOPS_PERMITTED=1
    )

    target_link_libraries(LightHostTests PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_events
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    # Same switch warning the app gets, for the same reason: the stub
    # processors switch over enums too.
    if(MSVC)
        target_compile_options(LightHostTests PRIVATE /w14062)
    endif()

    # One CTest entry runs the whole binary, which runs every category in
    # Tests/TestMain.cpp. Named for what it is rather than for one of the
    # categories inside it.
    add_test(NAME unit COMMAND LightHostTests)
    set_tests_properties(unit PROPERTIES LABELS unit TIMEOUT 120)

    # ── Headless startup/shutdown smoke test ────────────────────────────────────
    # Runs the real app: real device manager, real graph, real tray icon, real
    # message loop, real teardown. It inspects its own log and exits non-zero on
    # any problem, so CTest needs only the exit code. Notably this is the only
    # automated exercise of IconMenu's destructor ordering.
    #
    # Linux needs a display. Without one JUCE does not degrade gracefully: the
    # tray icon path dereferences a null X display and the process segfaults. So
    # the test is registered only when xvfb-run is available, and says so when it
    # is not, rather than being silently skipped.
    set(LIGHTHOST_SMOKE_LAUNCHER "")
    set(LIGHTHOST_SMOKE_AVAILABLE TRUE)

    if(UNIX AND NOT APPLE)
        find_program(XVFB_RUN xvfb-run)
        if(XVFB_RUN)
            set(LIGHTHOST_SMOKE_LAUNCHER ${XVFB_RUN} -a "--server-args=-screen 0 1280x1024x24")
        else()
            set(LIGHTHOST_SMOKE_AVAILABLE FALSE)
            message(WARNING
                "xvfb-run not found: the startup smoke test will not be registered. "
                "Install xvfb to enable it (the app needs an X display to create its tray icon).")
        endif()
    endif()

    # ── The window-creating half of the PluginWindow tests ──────────────────────
    # Separate from `unit` because constructing a DocumentWindow needs a display
    # server, and folded into the main run it would take the whole suite down on
    # a headless runner.
    #
    # NOT registered on Linux, and this is a real limitation rather than a
    # convenience. LightHostTests is a console app, and JUCE's X11 backend cannot
    # create a window from one: it dies with "BadAtom (invalid Atom parameter)"
    # on X_ChangeProperty before any assertion runs, even under xvfb with the
    # display working. The application itself is a GUI app, so it is unaffected —
    # the smoke tests open the real Preferences window under xvfb on this same
    # runner and pass, which is what covers Linux window creation.
    #
    # What is lost is narrow: the assertions in the PluginWindowGui category are
    # about host logic around windows — that a second Edit on a plugin with no
    # native editor reuses one window rather than opening a duplicate — and that
    # logic is a registry lookup with no platform component. It is checked on
    # Windows and macOS on every push.
    #
    # Making the test target a GUI app would fix it and cost more than it buys:
    # on Windows that switches the subsystem and stdout stops reaching the
    # terminal, so the other 1120 assertions would lose their output.
    if(APPLE OR WIN32)
        add_test(NAME unit-gui COMMAND LightHostTests --gui)
        set_tests_properties(unit-gui PROPERTIES LABELS unit TIMEOUT 120)
    else()
        message(STATUS
            "unit-gui is not registered on this platform: JUCE's X11 backend "
            "cannot create a window from a console app. Linux window creation is "
            "covered by the smoke tests, which run the real GUI app under xvfb.")
    endif()

    if(LIGHTHOST_SMOKE_AVAILABLE)
        add_test(NAME smoke-startup
                 COMMAND ${LIGHTHOST_SMOKE_LAUNCHER} $<TARGET_FILE:LightHost> -self-test)
        set_tests_properties(smoke-startup PROPERTIES LABELS smoke TIMEOUT 90)

        # A second, identical run. Each self-test uses a settings folder unique to
        # the run, so this is not a restore-from-previous-session check (the seeded
        # chain inside every run covers that); it is a repeat, which is what
        # catches a startup that only fails sometimes.
        add_test(NAME smoke-repeat
                 COMMAND ${LIGHTHOST_SMOKE_LAUNCHER} $<TARGET_FILE:LightHost> -self-test)
        set_tests_properties(smoke-repeat PROPERTIES LABELS smoke TIMEOUT 90
                             DEPENDS smoke-startup)

        # The multi-instance flag suffixes the settings filename; check it does not
        # break startup.
        add_test(NAME smoke-multi-instance
                 COMMAND ${LIGHTHOST_SMOKE_LAUNCHER} $<TARGET_FILE:LightHost>
                         -self-test -multi-instance=selftest)
        set_tests_properties(smoke-multi-instance PROPERTIES LABELS smoke TIMEOUT 90)
    endif()
endif()

# ── CMake presets integration ───────────────────────────────────────────────────
# Use cmake --preset=default to build, or cmake --preset=release for optimised

# ── Apply the warning gate ──────────────────────────────────────────────────────
# Last, so every source file is known, and PER FILE rather than per target.
#
# Per target does not work here, and the difference is not cosmetic. JUCE's
# CMake adds its module .cpp files to OUR target rather than building a library
# of its own -- the object files land in CMakeFiles/LightHost.dir/lib/juce/...
# -- so target_compile_options(LightHost PRIVATE -Werror) makes every warning
# in vendored JUCE an error too. Measured, not assumed: it stopped the build on
# juce_VSTPluginFormat.cpp's unused private field 'alreadyInside', which is
# Clang's complaint about somebody else's code and would have failed the macOS
# job after the push.
#
# Failing on a diagnostic in code the project is not going to edit teaches one
# thing only, which is to turn the switch off. So the flag goes on the files
# this repository is responsible for, and nowhere else.
#
# LIGHTHOST_WARNINGS_AS_ERRORS is defined above, with what it cost to turn on.
option(LIGHTHOST_WERROR "Treat compiler warnings as errors in Light Host's own sources" ON)

# Only .cpp files: the headers listed in target_sources are there so they show
# up in an IDE, and a header is diagnosed as part of whichever .cpp includes it.
set(LIGHTHOST_OWN_SOURCES
    Source/HostStartup.cpp
    Source/IconMenu.cpp
    Source/PluginWindow.cpp
    Source/PreferencesWindow.cpp
)

if(LIGHTHOST_BUILD_TESTS)
    file(GLOB LIGHTHOST_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/*.cpp")
    list(APPEND LIGHTHOST_OWN_SOURCES ${LIGHTHOST_TEST_SOURCES})
endif()

if(LIGHTHOST_WERROR)
    set_source_files_properties(${LIGHTHOST_OWN_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${LIGHTHOST_WARNINGS_AS_ERRORS}")
else()
    message(STATUS
        "LIGHTHOST_WERROR is OFF: warnings will not fail the build. This is a "
        "local escape hatch for bisecting against an older compiler, not a "
        "configuration to commit or to use in CI.")
endif()
