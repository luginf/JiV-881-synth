cmake_minimum_required(VERSION 3.22)
project(jiv881Android VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same JUCE tag the desktop build fetches (VirtualJiV.jucer / .github/workflows/main.yml) -
# already cloned to android/juce-src by settings.gradle before Gradle even gets this far, since
# the Java side (app/build.gradle's sourceSets) needs those same sources on disk too.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../juce-src" "${CMAKE_CURRENT_BINARY_DIR}/juce")

# The three photo/panel PNGs PanelSkin.cpp draws (BinaryData::jiv881_png/jiv881_panel_compact_png/
# jiv881_commands_png) - Projucer normally generates this BinaryData.cpp/h for the desktop build
# from VirtualJiV.jucer's Assets group; juce_add_binary_data reproduces the exact same generated
# symbol names from these same source files for this CMake-only Android target, so PanelSkin.cpp
# compiles completely unchanged.
juce_add_binary_data(JivPanelData
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Resources/jiv881.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Resources/jiv881_panel_compact.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Resources/jiv881_commands.png")

juce_add_gui_app(jiv881Android
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "DIY"
    PRODUCT_NAME "JiV-881"
)

# _juce_set_output_name (JUCEUtils.cmake) deliberately no-ops on Android, so PRODUCT_NAME above
# never reaches the actual .so filename there - has to be set directly instead. JUCE's generic
# Android Java glue (com.rmsl.juce.Java, see app/build.gradle's sourceSets comment) hardcodes
# System.loadLibrary("juce_jni"): the produced library has to be named to match, or it
# dlopen()s a name nothing on disk has (UnsatisfiedLinkError, "library "libjuce_jni.so" not
# found" - same gotcha the D110 Android port hit and documented in its own CMakeLists.txt).
set_target_properties(jiv881Android PROPERTIES OUTPUT_NAME "juce_jni")

# NDK r27's linker (lld) does not yet default to 16 KB ELF segment alignment (that only became
# the default starting with NDK r28) - without this flag libjuce_jni.so itself is 4 KB-aligned
# and trips Android's 16 KB page-size compatibility warning, same as the prebuilt
# libc++_shared.so worked around via ANDROID_STL=c++_static in app/build.gradle.
if(ANDROID)
    target_link_options(jiv881Android PRIVATE "-Wl,-z,max-page-size=16384")
endif()

set(JIV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Source")

# The real emulator core, panel/keyboard/sequencer UI and the tabbed PluginEditor - unchanged
# from the desktop plugin. PluginEditor.cpp/its tab classes are never actually shown by this
# app's own MainComponent (Main.cpp builds a phone-shaped UI directly out of PanelSkin/
# VirtualKeyboard/JivSequencerPanel/PatchBrowser instead - see Main.cpp's own top comment), but
# VirtualJVProcessor::createEditor() references VirtualJVEditor's constructor, so its whole
# dependency graph still has to be linked in even though it's never instantiated at runtime.
target_sources(jiv881Android PRIVATE
    src/main/cpp/Main.cpp
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
    # libresample (vendored, plain C) - pcm.cpp's own resample_open()/resample_process()/
    # resample_close() calls. Projucer's Makefile/Xcode/VS exporters pick these up
    # automatically by scanning the Source tree; CMake needs them named explicitly.
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
    ${JIV_SOURCE_DIR}/ui/widgets/VirtualKeyboard.cpp
)

target_include_directories(jiv881Android PRIVATE ${JIV_SOURCE_DIR})

# PluginProcessor.h (and everything that includes it) reaches JUCE through the desktop
# Projucer build's generated <JuceHeader.h> convenience header - juce_generate_juce_header()
# is JUCE's own CMake equivalent, producing the same kind of header (every module this target
# links against, pulled into one includable file) so that unchanged.
juce_generate_juce_header(jiv881Android)

target_compile_definitions(jiv881Android PRIVATE
    # The desktop Projucer build's own generated JuceLibraryCode/JuceHeader.h has never emitted
    # `using namespace juce;` (Projucer only does that under an opt-in project setting this
    # project doesn't have) - juce_generate_juce_header()'s CMake equivalent does by default,
    # which makes this project's own global-namespace `Button`/`Slider` widget classes
    # (Source/ui/widgets/Button.h/Slider.h) genuinely ambiguous against juce::Button/juce::Slider
    # wherever both are visible (EditCommonTab.h, PerformanceTab.h - pulled in transitively via
    # PluginEditor.h even though this app's own UI never uses those tabs). Matches the real
    # desktop build's behaviour exactly rather than special-casing the Android target's own code.
    DONT_SET_USING_JUCE_NAMESPACE=1
    # getName() returns this literally - normally generated into JucePluginDefines.h by
    # Projucer/juce_add_plugin, but juce_add_gui_app (this target - no plugin wrapper) never
    # defines it. Supplied directly rather than touching PluginProcessor.cpp.
    "JucePlugin_Name=\"JiV-881\""
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Not about push notifications at all here: this macro is also what activates
    # juce_Windowing_android.cpp's ONLY implementation of appOnResume()/appNewIntent(), two
    # native methods JuceActivity.java calls UNCONDITIONALLY from onResume()/onNewIntent()
    # regardless of whether the app uses push notifications. Without it those two native methods
    # have no implementation anywhere in the binary and the app crashes with
    # UnsatisfiedLinkError the first time onResume() runs (same gotcha the D110 Android port hit
    # - see its own CMakeLists.txt comment). JUCE_PUSH_NOTIFICATIONS=1 has to come with it: the
    # two functions those callbacks call only exist in juce_PushNotifications_android.cpp, which
    # juce_gui_extra.cpp only compiles in under this flag - no Firebase/FCM dependency involved.
    JUCE_PUSH_NOTIFICATIONS_ACTIVITY="com/rmsl/juce/JuceActivity"
    JUCE_PUSH_NOTIFICATIONS=1
)

target_link_libraries(jiv881Android PRIVATE
    JivPanelData
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
)

target_link_libraries(jiv881Android PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
