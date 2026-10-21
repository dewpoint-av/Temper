cmake_minimum_required(VERSION 3.21)
project(Temper VERSION 0.1.2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Disable VST2 compatibility to prevent conflicts
add_definitions(-DJUCE_VST3_CAN_REPLACE_VST2=0)
add_definitions(-DJUCE_IGNORE_VST3_MISMATCHED_PARAMETER_ID_WARNING=1)

# ---- Vendored dependencies (copied into the tree, no network needed) ------
add_subdirectory(JUCE)
add_subdirectory(clap-juce-extensions)

# ---- The plugin ------------------------------------------------------------
set(TEMPER_PLUGIN_FORMATS VST3)
if(APPLE)
    list(APPEND TEMPER_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(Temper
    COMPANY_NAME "Dewpoint AV"
    COMPANY_WEBSITE "https://dewpoint.live"
    COMPANY_EMAIL "anthony@dewpoint.live"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Dwpt
    PLUGIN_CODE Tmpr
    FORMATS ${TEMPER_PLUGIN_FORMATS}
    PRODUCT_NAME "Temper"
    BUNDLE_ID "com.dewpoint.Temper"
)

clap_juce_extensions_plugin(TARGET Temper
    CLAP_ID "live.dewpoint.temper"
    CLAP_FEATURES audio-effect equalizer stereo)

juce_generate_juce_header(Temper)

# Embedded Monument Extended logotype fonts.
juce_add_binary_data(TemperBinaryData
    SOURCES
        Source/MonumentExtendedUltrabold.otf
        Source/MonumentExtendedRegular.otf)

# ---- Source files ----------------------------------------------------------
target_sources(Temper
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/dsp/SpectralSuppressor.cpp
)

target_include_directories(Temper
    PRIVATE
        Source
)

target_compile_definitions(Temper
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# ---- Link JUCE modules -----------------------------------------------------
target_link_libraries(Temper
    PRIVATE
        TemperBinaryData
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_basics
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_gui_basics
        juce::juce_graphics
        juce::juce_data_structures
        juce::juce_events
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ---- Compiler flags --------------------------------------------------------
if(MSVC)
    target_compile_options(Temper PRIVATE /W4)
else()
    target_compile_options(Temper PRIVATE -Wall -Wextra)
endif()

# ---- Packaging / staging (Windows) -----------------------------------------
if(WIN32)
    add_custom_command(TARGET Temper_CLAP POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Temper_CLAP>"
            "${CMAKE_BINARY_DIR}/dist/Temper.clap"
        COMMENT "Staging Temper.clap")

    add_custom_command(TARGET Temper_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/Temper_artefacts/$<CONFIG>/VST3/Temper.vst3"
            "${CMAKE_BINARY_DIR}/dist/Temper.vst3"
        COMMENT "Staging Temper.vst3")
endif()
