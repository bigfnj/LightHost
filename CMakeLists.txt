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

project(LightHost
    VERSION 4.0.3
    DESCRIPTION "Lightweight VST/AU/VST3 Audio Plugin Host"
    HOMEPAGE_URL "https://github.com/BigFNj"
    LANGUAGES C CXX
)

# ── C++23 standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── JUCE ────────────────────────────────────────────────────────────────────────
# Expects JUCE 8.0.12+ at lib/juce (or override via -DJUCE_DIR=<path>)
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
    Source/SelfTest.hpp
    Source/IconMenu.cpp
    Source/PluginWindow.cpp
    Source/PreferencesWindow.cpp
)

# ── Binary resources ────────────────────────────────────────────────────────────
juce_add_binary_data(LightHostData SOURCES
    Resources/MicrophoneIcon.png
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
target_include_directories(LightHost PRIVATE
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
    target_compile_definitions(LightHost PRIVATE
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        NTDDI_VERSION=0x0A00000C
    )

    # Enable latest MSVC features
    target_compile_options(LightHost PRIVATE
        /W4             # Warning level 4
        /permissive-    # Standards conformance
        /Zc:__cplusplus # Report correct __cplusplus value
        /Zc:preprocessor # Conforming preprocessor
        /utf-8          # Source and execution charset UTF-8
        /wd5105         # Suppress C5105 from JUCE's bundled HarfBuzz ('defined' in macro expansion)
    )

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
        Tests/TestMain.cpp
        Tests/GraphRenderTests.cpp
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
    )

    target_link_libraries(LightHostTests PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_events
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    add_test(NAME graphrender COMMAND LightHostTests)
    set_tests_properties(graphrender PROPERTIES LABELS unit TIMEOUT 120)

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

    if(LIGHTHOST_SMOKE_AVAILABLE)
        add_test(NAME smoke-startup
                 COMMAND ${LIGHTHOST_SMOKE_LAUNCHER} $<TARGET_FILE:LightHost> -self-test)
        set_tests_properties(smoke-startup PROPERTIES LABELS smoke TIMEOUT 90)

        # Second run against the settings the first one wrote, exercising the
        # restore path rather than only first-launch behaviour.
        add_test(NAME smoke-restore
                 COMMAND ${LIGHTHOST_SMOKE_LAUNCHER} $<TARGET_FILE:LightHost> -self-test)
        set_tests_properties(smoke-restore PROPERTIES LABELS smoke TIMEOUT 90
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
