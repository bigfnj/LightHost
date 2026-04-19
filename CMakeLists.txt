cmake_minimum_required(VERSION 3.28)

# ── Windows SDK version (Windows 11 26100) ──────────────────────────────────────
# Must be set before project() so the VS generator picks it up for WindowsTargetPlatformVersion.
set(CMAKE_SYSTEM_VERSION "10.0.26100.0" CACHE STRING "Windows SDK version" FORCE)

project(LightHost
    VERSION 3.8.1
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

# ── CMake presets integration ───────────────────────────────────────────────────
# Use cmake --preset=default to build, or cmake --preset=release for optimised
