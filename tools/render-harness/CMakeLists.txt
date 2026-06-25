cmake_minimum_required(VERSION 3.22)
project(PlugNspectrTools VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE lives outside the repo; override with -DJUCE_DIR=... if needed.
set(JUCE_DIR "C:/Program Files/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

set(POST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../PlugNspectrPost")

# ---------------------------------------------------------------------------
# Headless render harness: instantiates the PlugNspectrPost editor, injects
# synthetic audio via the processor's test seam, and dumps PNG frames — no DAW
# required. Compiles the plugin's UI/processor sources directly (no plugin
# wrapper), so it links against the same code that ships in the VST3.
#
# NOTE: relies on the Projucer-generated BinaryData (the logo), i.e. run a
# normal plugin build once first so PlugNspectrPost/JuceLibraryCode/BinaryData.*
# exist. They are .gitignored, so a fresh clone must build the plugin first.
# ---------------------------------------------------------------------------
juce_add_console_app(RenderHarness PRODUCT_NAME "PnsRenderHarness")
juce_generate_juce_header(RenderHarness)

target_sources(RenderHarness PRIVATE
    Main.cpp
    "${POST_DIR}/Source/PluginProcessor.cpp"
    "${POST_DIR}/Source/PluginEditor.cpp"
    "${POST_DIR}/JuceLibraryCode/BinaryData.cpp")

target_include_directories(RenderHarness PRIVATE
    "${POST_DIR}/Source"
    "${POST_DIR}/JuceLibraryCode")

target_compile_definitions(RenderHarness PRIVATE
    JucePlugin_Name="PlugNspectrPost"
    JUCE_STANDALONE_APPLICATION=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(RenderHarness PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Standalone app: the full PlugNspectrPost UI as a launchable .exe with its own
# audio device — instant manual iteration without a DAW. Built from the same
# sources; the Projucer/VST3 project is left completely untouched.
# ---------------------------------------------------------------------------
juce_add_plugin(PnsPostStandalone
    PRODUCT_NAME "PlugNspectrPost"
    COMPANY_NAME "Biltroy Audio"
    PLUGIN_MANUFACTURER_CODE Bilt
    PLUGIN_CODE BNSQ
    FORMATS Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE)
juce_generate_juce_header(PnsPostStandalone)

target_sources(PnsPostStandalone PRIVATE
    "${POST_DIR}/Source/PluginProcessor.cpp"
    "${POST_DIR}/Source/PluginEditor.cpp"
    "${POST_DIR}/JuceLibraryCode/BinaryData.cpp")

target_include_directories(PnsPostStandalone PRIVATE
    "${POST_DIR}/Source"
    "${POST_DIR}/JuceLibraryCode")

target_compile_definitions(PnsPostStandalone PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(PnsPostStandalone PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Unit tests (doctest). Two console exes — Post engines and Pre generators are
# split so the duplicate createPluginFilter() symbols don't clash. Both are
# EDITOR-FREE: they compile only the processor sources (PNS_HEADLESS_TESTS
# stubs out createEditor), so no PluginEditor / BinaryData / Projucer is needed.
# ---------------------------------------------------------------------------
set(TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
set(PRE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../../PlugNspectrPre")
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# --- Post DSP engines + shared-memory ABI ---
juce_add_console_app(PnsTests PRODUCT_NAME "PnsTests")
juce_generate_juce_header(PnsTests)
target_sources(PnsTests PRIVATE
    "${TESTS_DIR}/test_main.cpp"
    "${TESTS_DIR}/test_linear.cpp"
    "${TESTS_DIR}/test_dynamics.cpp"
    "${TESTS_DIR}/test_envelope.cpp"
    "${TESTS_DIR}/test_thd.cpp"
    "${TESTS_DIR}/test_channel_rms.cpp"
    "${TESTS_DIR}/test_shared_abi.cpp"
    "${TESTS_DIR}/test_seqlock.cpp"
    "${POST_DIR}/Source/PluginProcessor.cpp")
target_include_directories(PnsTests PRIVATE "${POST_DIR}/Source" "${TESTS_DIR}")
target_compile_definitions(PnsTests PRIVATE
    JucePlugin_Name="PlugNspectrPost"
    PNS_HEADLESS_TESTS=1
    PNS_REPO_ROOT="${REPO_ROOT}"
    JUCE_STANDALONE_APPLICATION=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(PnsTests PRIVATE
    juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# --- Pre stimulus generators ---
juce_add_console_app(PnsTestsPre PRODUCT_NAME "PnsTestsPre")
juce_generate_juce_header(PnsTestsPre)
target_sources(PnsTestsPre PRIVATE
    "${TESTS_DIR}/test_main.cpp"
    "${TESTS_DIR}/test_pre_generators.cpp"
    "${PRE_DIR}/Source/PluginProcessor.cpp")
target_include_directories(PnsTestsPre PRIVATE "${PRE_DIR}/Source" "${TESTS_DIR}")
target_compile_definitions(PnsTestsPre PRIVATE
    JucePlugin_Name="PlugNspectrPre"
    PNS_HEADLESS_TESTS=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(PnsTestsPre PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
