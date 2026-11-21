cmake_minimum_required(VERSION 3.22)
project(ArrayLab VERSION 0.8.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Engine: plain C++, no JUCE, so it can be unit-tested on its own.
add_library(arraylab_engine STATIC
    src/Fft.cpp
    src/DelayEstimator.cpp
    src/PhaseFir.cpp)
target_include_directories(arraylab_engine PUBLIC src)
set_target_properties(arraylab_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

juce_add_plugin(ArrayLab
    COMPANY_NAME "ArrayLab"
    PRODUCT_NAME "ArrayLab"
    PLUGIN_MANUFACTURER_CODE Arlb
    PLUGIN_CODE Alp1
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Tools Analyzer
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(ArrayLab PRIVATE
    src/Hub.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/StageView.cpp
    src/Inspector.cpp
    src/CheckView.cpp
    src/DetailView.cpp
    src/StripView.cpp)

target_compile_definitions(ArrayLab PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(ArrayLab
    PRIVATE
        arraylab_engine
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

enable_testing()
add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE arraylab_engine)
add_test(NAME engine_test COMMAND engine_test)

juce_add_console_app(link_test PRODUCT_NAME "link_test")
target_sources(link_test PRIVATE tests/link_test.cpp)
target_link_libraries(link_test PRIVATE ArrayLab juce::juce_audio_utils juce::juce_dsp)
add_test(NAME link_test COMMAND link_test)
