cmake_minimum_required(VERSION 3.22)
project(BassRandomizer VERSION 1.0.0)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# On Windows the system VST3 folder needs admin rights, so copy manually there.
if(WIN32)
    set(COPY_AFTER_BUILD FALSE)
else()
    set(COPY_AFTER_BUILD TRUE)
endif()

juce_add_plugin(BassRandomizer
    COMPANY_NAME "YourName"
    PLUGIN_MANUFACTURER_CODE Yrnm
    PLUGIN_CODE Bsrd
    FORMATS VST3
    PRODUCT_NAME "Bass Randomizer"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Instrument
    COPY_PLUGIN_AFTER_BUILD ${COPY_AFTER_BUILD}
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(BassRandomizer PRIVATE
    Source/PluginProcessor.cpp)

target_compile_definitions(BassRandomizer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(BassRandomizer
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
