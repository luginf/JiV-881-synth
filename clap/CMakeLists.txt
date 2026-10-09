# CLAP build of JiV-881 (Linux release asset). The desktop product is built by Projucer
# (VirtualJiV.jucer -> Builds/LinuxMakefile), which has no CLAP exporter, so this small CMake
# project compiles the same sources a second time with JUCE's CMake API plus
# free-audio/clap-juce-extensions - modelled on android/app/CMakeLists.txt, which already builds
# this source tree with CMake. Only the JiV881_CLAP target is meant to be built:
#
#   cmake -S clap -B /tmp/jiv-clap -DCMAKE_BUILD_TYPE=Release
#   cmake --build /tmp/jiv-clap --target JiV881_CLAP -j8      # -> JiV-881.clap
cmake_minimum_required(VERSION 3.22)
project(JiV881Clap VERSION 1.4.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" "${CMAKE_CURRENT_BINARY_DIR}/juce")
else()
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.15)
    FetchContent_MakeAvailable(JUCE)
endif()
FetchContent_Declare(clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git GIT_TAG main)
FetchContent_MakeAvailable(clap-juce-extensions)

set(JIV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Source")

# The BinaryData symbols Projucer generates from VirtualJiV.jucer's Assets group.
juce_add_binary_data(JivBinaryData
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Resources/jiv881.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Resources/jiv881_panel_compact.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Resources/jiv881_commands.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/VirtualJiV Logo.svg")

juce_add_plugin(JiV881
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "luginf"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    PLUGIN_MANUFACTURER_CODE Lgnf
    PLUGIN_CODE JI81
    FORMATS VST3
    PRODUCT_NAME "JiV-881"
    PLUGIN_NAME "JiV-881")

target_sources(JiV881 PRIVATE
    ${JIV_SOURCE_DIR}/PluginProcessor.cpp
    ${JIV_SOURCE_DIR}/PluginEditor.cpp
    ${JIV_SOURCE_DIR}/rom.cpp
    ${JIV_SOURCE_DIR}/sha1.cpp
    ${JIV_SOURCE_DIR}/emulator/lcd.cpp
    ${JIV_SOURCE_DIR}/emulator/mcu.cpp
    ${JIV_SOURCE_DIR}/emulator/mcu_interrupt.cpp
    ${JIV_SOURCE_DIR}/emulator/mcu_opcodes.cpp
    ${JIV_SOURCE_DIR}/emulator/mcu_timer.cpp
    ${JIV_SOURCE_DIR}/emulator/pcm.cpp
    ${JIV_SOURCE_DIR}/emulator/submcu.cpp
    ${JIV_SOURCE_DIR}/emulator/resample/resample.c
    ${JIV_SOURCE_DIR}/emulator/resample/resamplesubs.c
    ${JIV_SOURCE_DIR}/emulator/resample/filterkit.c
    ${JIV_SOURCE_DIR}/sequencer/JivSequencerEngine.cpp
    ${JIV_SOURCE_DIR}/sequencer/JivSequencerSongsFile.cpp
    ${JIV_SOURCE_DIR}/sequencer/JivSequencerPanel.cpp
    ${JIV_SOURCE_DIR}/sequencer/JivSequencerGridPanel.cpp
    ${JIV_SOURCE_DIR}/sequencer/JivSequencerRetroPanel.cpp
    ${JIV_SOURCE_DIR}/DotMatrixFont.cpp
    ${JIV_SOURCE_DIR}/ui/PanelSkin.cpp
    ${JIV_SOURCE_DIR}/ui/PatchBrowser.cpp
    ${JIV_SOURCE_DIR}/ui/PerformanceTab.cpp
    ${JIV_SOURCE_DIR}/ui/InterfaceTab.cpp
    ${JIV_SOURCE_DIR}/ui/EditCommonTab.cpp
    ${JIV_SOURCE_DIR}/ui/EditToneTab.cpp
    ${JIV_SOURCE_DIR}/ui/EditRhythmTab.cpp
    ${JIV_SOURCE_DIR}/ui/SettingsTab.cpp
    ${JIV_SOURCE_DIR}/ui/widgets/LCDisplay.cpp
    ${JIV_SOURCE_DIR}/ui/widgets/VirtualKeyboard.cpp)

target_include_directories(JiV881 PRIVATE ${JIV_SOURCE_DIR})
juce_generate_juce_header(JiV881)

target_compile_definitions(JiV881 PUBLIC
    DONT_SET_USING_JUCE_NAMESPACE=1   # same as the Projucer build (global Button/Slider widgets)
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(JiV881
    PRIVATE JivBinaryData juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags)

clap_juce_extensions_plugin(TARGET JiV881
    CLAP_ID "com.luginf.jiv881"
    CLAP_FEATURES instrument synthesizer)
