cmake_minimum_required(VERSION 3.22)
project(PlugNspectr VERSION 1.0.3 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Cross-platform plugin build (Windows + macOS) for both PlugNspectr plugins.
# This is the build used for macOS / CI; the Projucer .jucer / .sln projects are
# kept for the existing local Windows workflow. The editor is BinaryData-free
# (logo lives in Source/PnsLogoData.h), so no Projucer step is needed here.
#
#   cmake -B build -DJUCE_DIR=<path-to-JUCE>
#   cmake --build build --config Release
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    # Universal binary: runs on both Apple Silicon and Intel Macs.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

# JUCE lives outside the repo; override with -DJUCE_DIR=... (CI passes a checkout).
set(JUCE_DIR "C:/Program Files/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

# VST3 everywhere; AU additionally on macOS (required by Logic, expected on Mac).
set(PNS_FORMATS VST3)
if(APPLE)
    list(APPEND PNS_FORMATS AU)
endif()

function(pns_add_plugin target product code dir)
    juce_add_plugin(${target}
        PRODUCT_NAME           "${product}"
        COMPANY_NAME           "Biltroy Audio"
        BUNDLE_ID              "com.biltroyaudio.${target}"
        PLUGIN_MANUFACTURER_CODE Bilt
        PLUGIN_CODE            ${code}
        FORMATS                ${PNS_FORMATS}
        IS_SYNTH               FALSE
        NEEDS_MIDI_INPUT       FALSE
        NEEDS_MIDI_OUTPUT      FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES        Analyzer
        AU_MAIN_TYPE           kAudioUnitType_Effect)

    juce_generate_juce_header(${target})

    target_sources(${target} PRIVATE
        "${dir}/Source/PluginProcessor.cpp"
        "${dir}/Source/PluginEditor.cpp")

    target_include_directories(${target} PRIVATE "${dir}/Source")

    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(${target} PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endfunction()

pns_add_plugin(PlugNspectrPre  "PlugNspectrPre"  BNSP "${CMAKE_CURRENT_SOURCE_DIR}/PlugNspectrPre")
pns_add_plugin(PlugNspectrPost "PlugNspectrPost" BNSQ "${CMAKE_CURRENT_SOURCE_DIR}/PlugNspectrPost")
