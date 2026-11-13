cmake_minimum_required(VERSION 3.25)  # vendored WDSP requires 3.25; Pi OS Bookworm ships 3.25.1
project(ExpertSDR3Audio VERSION 0.1.0 LANGUAGES C CXX)

# The release pipeline stamps the real version here. Everything that shows a version,
# from --version to the package file names, reads this one value.
set(ESA_VERSION "${PROJECT_VERSION}" CACHE STRING "Version stamped into binaries and packages")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_NR4  "NR4 (libspecbleach). MSVC's own compiler cannot build it; clang-cl can." ON)
option(ENABLE_DFNR "DFNR (DeepFilterNet3). Needs vendor/aethersdr/scripts/setup/setup-deepfilter.*" ON)
option(ENABLE_NNR  "NNR (WDSP neural NR)." ON)
option(BUILD_TESTS "Build the test suite." ON)
option(ESA_STATIC_PORTAUDIO "Build PortAudio from source and link it statically, ignoring any system copy." OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)
qt_standard_project_setup()

set(AETHER_DIR ${CMAKE_SOURCE_DIR}/vendor/aethersdr)
set(AETHER_TP  ${AETHER_DIR}/third_party)

# ── FFTW (double for NR2/NNR, float for NR4) ─────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFTW3 QUIET fftw3)
    pkg_check_modules(FFTW3F QUIET fftw3f)
    pkg_check_modules(PORTAUDIO QUIET portaudio-2.0)
endif()
if(NOT FFTW3_FOUND)  # Windows/vcpkg has no pkg-config
    find_path(FFTW3_INCLUDE_DIRS fftw3.h REQUIRED)
    find_library(FFTW3_LIBRARIES NAMES fftw3 fftw3-3 libfftw3-3 REQUIRED)
    find_library(FFTW3F_LIBRARIES NAMES fftw3f fftw3f-3 libfftw3f-3)
    set(FFTW3_FOUND TRUE)
    set(FFTW3_LINK_LIBRARIES ${FFTW3_LIBRARIES})
    set(FFTW3F_LINK_LIBRARIES ${FFTW3F_LIBRARIES})
endif()

# ── PortAudio ────────────────────────────────────────────────────────────────
if(PORTAUDIO_FOUND AND NOT ESA_STATIC_PORTAUDIO)
    add_library(esa_portaudio INTERFACE)
    target_include_directories(esa_portaudio INTERFACE ${PORTAUDIO_INCLUDE_DIRS})
    target_link_libraries(esa_portaudio INTERFACE ${PORTAUDIO_LINK_LIBRARIES})
else()
    include(FetchContent)
    message(STATUS "Building PortAudio v19.7.0 from source")
    set(PA_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_STATIC ON CACHE BOOL "" FORCE)
    # JACK's library is not something a package can bundle or assume: an AppImage linking it
    # fails to start on nearly every desktop. Sound still reaches PipeWire and PulseAudio
    # through ALSA.
    set(PA_USE_JACK OFF CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # its CMakeLists predates CMake 4
    FetchContent_Declare(portaudio
        GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
        GIT_TAG v19.7.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(portaudio)
    add_library(esa_portaudio INTERFACE)
    target_link_libraries(esa_portaudio INTERFACE portaudio_static)
    # Built in, so its MIT notice goes with every copy (Licences, below).
    set(ESA_PORTAUDIO_LICENSE ${portaudio_SOURCE_DIR}/LICENSE.txt)
endif()

# ── RNNoise (RN2) ────────────────────────────────────────────────────────────
set(RN ${AETHER_TP}/rnnoise)
set(RNNOISE_SOURCES
    ${RN}/src/denoise.c ${RN}/src/celt_lpc.c ${RN}/src/kiss_fft.c ${RN}/src/pitch.c
    ${RN}/src/rnn.c ${RN}/src/nnet.c ${RN}/src/nnet_default.c ${RN}/src/rnnoise_data.c
    ${RN}/src/rnnoise_tables.c ${RN}/src/parse_lpcnet_weights.c)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    set(RN_X86 TRUE)
    list(APPEND RNNOISE_SOURCES
        ${RN}/src/x86/x86cpu.c ${RN}/src/x86/x86_dnn_map.c
        ${RN}/src/x86/nnet_sse4_1.c ${RN}/src/x86/nnet_avx2.c)
endif()
add_library(aether_rnnoise STATIC ${RNNOISE_SOURCES})
target_include_directories(aether_rnnoise PUBLIC ${RN}/include PRIVATE ${RN}/src)
set_target_properties(aether_rnnoise PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(RN_X86)
    target_include_directories(aether_rnnoise PRIVATE ${RN}/src/x86)
    target_compile_definitions(aether_rnnoise PRIVATE RNN_ENABLE_X86_RTCD=1)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        set_source_files_properties(${RN}/src/x86/nnet_sse4_1.c PROPERTIES COMPILE_DEFINITIONS __SSE4_1__)
        set_source_files_properties(${RN}/src/x86/nnet_avx2.c PROPERTIES
            COMPILE_FLAGS /arch:AVX2 COMPILE_DEFINITIONS __AVX2__)
    elseif(MSVC)  # clang-cl: MSVC's CPU detection, but clang needs the ISA flags per file
        set_source_files_properties(${RN}/src/x86/nnet_sse4_1.c PROPERTIES COMPILE_FLAGS -msse4.1)
        set_source_files_properties(${RN}/src/x86/nnet_avx2.c PROPERTIES COMPILE_FLAGS "-mavx -mfma -mavx2")
    else()
        target_compile_definitions(aether_rnnoise PRIVATE CPU_INFO_BY_C=1)
        set_source_files_properties(${RN}/src/x86/nnet_sse4_1.c PROPERTIES COMPILE_FLAGS -msse4.1)
        set_source_files_properties(${RN}/src/x86/nnet_avx2.c PROPERTIES COMPILE_FLAGS "-mavx -mfma -mavx2")
    endif()
endif()
target_compile_options(aether_rnnoise PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(aether_rnnoise PUBLIC ${MATH_LIBRARY})
endif()

# ── libspecbleach (NR4) ──────────────────────────────────────────────────────
if(ENABLE_NR4 AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    message(WARNING "NR4 disabled: libspecbleach uses C99 VLAs, which cl.exe rejects. Configure with -T ClangCL to enable it.")
    set(ENABLE_NR4 OFF)
endif()
if(ENABLE_NR4 AND NOT FFTW3F_LINK_LIBRARIES)
    message(WARNING "NR4 disabled: single-precision FFTW (fftw3f) not found.")
    set(ENABLE_NR4 OFF)
endif()
if(ENABLE_NR4)
    file(GLOB_RECURSE SPECBLEACH_SOURCES CONFIGURE_DEPENDS ${AETHER_TP}/libspecbleach/src/*.c)
    add_library(aether_specbleach STATIC ${SPECBLEACH_SOURCES})
    target_include_directories(aether_specbleach
        PUBLIC ${AETHER_TP}/libspecbleach/include
        PRIVATE ${AETHER_TP}/libspecbleach/src ${FFTW3F_INCLUDE_DIRS} ${FFTW3_INCLUDE_DIRS})
    target_link_libraries(aether_specbleach PUBLIC ${FFTW3F_LINK_LIBRARIES})
    target_compile_options(aether_specbleach PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
    set_target_properties(aether_specbleach PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()

# ── WDSP (NNR) ───────────────────────────────────────────────────────────────
if(ENABLE_NNR)
    # WDSP links ${FFTW3_LIBRARIES} with a private search path, which does not reach
    # whatever links WDSP in turn. Hand it full paths so the bare name never escapes.
    set(FFTW3_LIBRARIES ${FFTW3_LINK_LIBRARIES})
    add_subdirectory(${AETHER_TP}/wdsp ${CMAKE_BINARY_DIR}/aether_wdsp EXCLUDE_FROM_ALL)
    if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # WDSP hands `volatile int*` to Interlocked*(), which take `volatile long*`. They are
        # the same size on Windows, so this is harmless, but recent clang-cl rejects it.
        target_compile_options(aether_wdsp PRIVATE -Wno-incompatible-pointer-types)
    endif()
endif()

# ── DeepFilterNet3 (DFNR) ────────────────────────────────────────────────────
if(ENABLE_DFNR)
    set(DF_DIR ${AETHER_TP}/deepfilter)
    if(WIN32)
        set(DF_LIB_DIR ${DF_DIR}/lib/windows-x86_64)
        set(DFNR_DLL ${DF_LIB_DIR}/deepfilter.dll)
        if(EXISTS ${DF_LIB_DIR}/deepfilter.dll.lib)
            set(DFNR_LIB ${DF_LIB_DIR}/deepfilter.dll.lib)
        else()
            set(DFNR_LIB ${DF_LIB_DIR}/deepfilter.lib)
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
            set(DF_LIB_DIR ${DF_DIR}/lib/darwin-arm64)
        else()
            set(DF_LIB_DIR ${DF_DIR}/lib/darwin-x86_64)
        endif()
        set(DFNR_LIB ${DF_LIB_DIR}/libdeepfilter.a)
    else()
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
            set(DF_LIB_DIR ${DF_DIR}/lib/linux-aarch64)
        else()
            set(DF_LIB_DIR ${DF_DIR}/lib/linux-x86_64)
        endif()
        set(DFNR_LIB ${DF_LIB_DIR}/libdeepfilter.a)
    endif()
    if(EXISTS ${DFNR_LIB})
        message(STATUS "DFNR enabled: ${DFNR_LIB}")
    else()
        message(STATUS "DFNR disabled: run vendor/aethersdr/scripts/setup/setup-deepfilter.sh (from vendor/aethersdr) to enable")
        set(ENABLE_DFNR OFF)
    endif()
endif()

# ── The AetherRX modules, compiled unmodified from vendor/ ───────────────────
set(CORE ${AETHER_DIR}/src/core)
set(AETHER_SOURCES
    ${CORE}/ClientEq.cpp ${CORE}/ClientGate.cpp ${CORE}/ClientComp.cpp
    ${CORE}/ClientPhaseRotator.cpp ${CORE}/ClientTube.cpp ${CORE}/ClientPudu.cpp
    ${CORE}/Resampler.cpp ${CORE}/RNNoiseFilter.cpp ${CORE}/SpectralNR.cpp
    ${CORE}/dsp/FftwPlannerLock.cpp
    src/dsp/AetherShim.cpp)
if(ENABLE_NR4)
    list(APPEND AETHER_SOURCES ${CORE}/SpecbleachFilter.cpp)
endif()
if(APPLE)
    list(APPEND AETHER_SOURCES ${CORE}/MacNRFilter.cpp)
endif()
if(ENABLE_DFNR)
    list(APPEND AETHER_SOURCES ${CORE}/DeepFilterFilter.cpp)
endif()
if(ENABLE_NNR)
    list(APPEND AETHER_SOURCES ${CORE}/NnrFilter.cpp)
endif()

add_library(aether_dsp STATIC ${AETHER_SOURCES})
set_target_properties(aether_dsp PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(aether_dsp
    PUBLIC ${AETHER_DIR}/src ${CORE} ${FFTW3_INCLUDE_DIRS}
    PRIVATE
        src/dsp/shim    # supplies the one upstream header not vendored (LogManager.h)
        ${AETHER_TP}/r8brain
        ${AETHER_TP}/deepfilter/include)
target_link_libraries(aether_dsp PUBLIC Qt6::Core aether_rnnoise ${FFTW3_LINK_LIBRARIES})
target_compile_definitions(aether_dsp PUBLIC HAVE_FFTW3
    $<$<BOOL:${WIN32}>:FFTW_DLL _USE_MATH_DEFINES NOMINMAX>)
if(ENABLE_NR4)
    target_compile_definitions(aether_dsp PUBLIC HAVE_SPECBLEACH)
    target_link_libraries(aether_dsp PUBLIC aether_specbleach)
endif()
if(APPLE)
    target_link_libraries(aether_dsp PUBLIC "-framework Accelerate")
endif()
if(ENABLE_NNR)
    target_compile_definitions(aether_dsp PUBLIC HAVE_NNR)
    target_link_libraries(aether_dsp PUBLIC aether::wdsp)
endif()
if(ENABLE_DFNR)
    target_compile_definitions(aether_dsp PUBLIC HAVE_DFNR)
    target_link_libraries(aether_dsp PUBLIC ${DFNR_LIB})
    if(WIN32)
        target_link_libraries(aether_dsp PUBLIC ws2_32 bcrypt userenv ntdll)
    elseif(APPLE)
        target_link_libraries(aether_dsp PUBLIC "-framework Security" "-framework CoreFoundation")
    else()
        target_link_libraries(aether_dsp PUBLIC pthread dl m)
    endif()
endif()

# ── The AetherRX pages, compiled unmodified from vendor/ ─────────────────────
# The seven stage panels and their widgets, the rail and the theme, as
# scripts/sync-aether.sh copied them from the list in scripts/aether-gui-files.txt.
# They are built against our stand-ins for the three upstream headers they need but
# that are not vendored (src/gui/aether/shim/core/), and against forked copies of the
# few files that need changing to build here (src/gui/aether/fork/). Appearance.cpp,
# ours, gives our window the same theme, font and ground.
set(AETHER_GUI_DIR ${AETHER_DIR}/src/gui)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/scripts/aether-gui-files.txt)
file(STRINGS ${CMAKE_SOURCE_DIR}/scripts/aether-gui-files.txt AETHER_GUI_SOURCES REGEX "\\.cpp$")
list(FILTER AETHER_GUI_SOURCES EXCLUDE REGEX "^#")
list(TRANSFORM AETHER_GUI_SOURCES PREPEND ${AETHER_GUI_DIR}/)

add_library(aether_gui STATIC
    ${AETHER_GUI_SOURCES}
    ${CORE}/ThemeManager.cpp
    ${CORE}/ThemeSeedGenerated.cpp     # the compiled-in theme ThemeManager starts from
    src/gui/aether/shim/core/AudioEngine.h  # a QObject with no .cpp of its own name, so moc is told
    src/gui/aether/AudioEngineShim.cpp
    src/gui/aether/AppSettingsShim.cpp
    src/gui/aether/LogShim.cpp
    src/gui/aether/Appearance.cpp
    src/gui/aether/Guards.cpp
    src/gui/aether/fork/CanonGround.cpp
    src/gui/aether/fork/NrGainStrip.cpp)
target_include_directories(aether_gui PUBLIC
    src/gui/aether/shim       # "core/AudioEngine.h", as the pages spell it
    src/gui/aether/shim/core  # "AppSettings.h" and "LogManager.h", as ThemeManager.cpp does
    src/gui/aether/fork       # a fork is included by the name its upstream file had
    ${AETHER_DIR}/src ${AETHER_GUI_DIR} ${CORE})
target_link_libraries(aether_gui PUBLIC aether_dsp Qt6::Widgets)
# The one theme the pages are drawn in (ThemeManager scans :/themes/), and the font it
# names. Qt links a static library's resources into whatever links the library;
# test_aether_gui checks they arrive.
qt_add_resources(aether_gui "aether_theme"
    PREFIX "/themes"
    BASE ${AETHER_DIR}/resources/themes
    FILES ${AETHER_DIR}/resources/themes/default-dark.json)
qt_add_resources(aether_gui "aether_fonts"
    PREFIX "/fonts"
    BASE ${AETHER_TP}/inter
    FILES ${AETHER_TP}/inter/Inter-Regular.ttf ${AETHER_TP}/inter/Inter-Bold.ttf)
# The tick in a ticked check box, which a style sheet cannot draw; Qt picks @2x or @3x
# on a sharper screen. Upstream's tick, painted by fork/tools/make-check-ticks.cpp (see
# fork/README.md).
set(CANON_TICKS ${CMAKE_SOURCE_DIR}/src/gui/aether/fork)
qt_add_resources(aether_gui "canon_ticks"
    PREFIX "/canon"
    BASE ${CANON_TICKS}
    FILES
        ${CANON_TICKS}/check-tick.png ${CANON_TICKS}/check-tick@2x.png
        ${CANON_TICKS}/check-tick@3x.png ${CANON_TICKS}/check-tick-disabled.png
        ${CANON_TICKS}/check-tick-disabled@2x.png ${CANON_TICKS}/check-tick-disabled@3x.png)

# ── Application core (everything except the GUI, shared with tests) ──────────
add_library(esa_core STATIC
    src/tci/TciProtocol.cpp src/tci/TciClient.cpp src/tci/TciDiscovery.cpp src/tci/MuteController.cpp
    src/dsp/RxChain.cpp src/dsp/NrRegistry.cpp
    src/audio/PlaybackBuffer.cpp src/audio/PortAudioSink.cpp
    src/app/Engine.cpp src/app/Profiles.cpp src/app/Settings.cpp)
target_include_directories(esa_core PUBLIC src)
target_link_libraries(esa_core PUBLIC
    Qt6::Core Qt6::Network Qt6::WebSockets aether_dsp esa_portaudio)

# ── Recording core: WAV files, the disk thread, PLAY's reader ─────────────────
# Its own block so it re-applies onto a restructured build: wherever the core library
# ends up, these four sources go with it. Qt Core only; the test is test_recorder.
target_sources(esa_core PRIVATE
    src/app/WavFile.cpp src/app/Recorder.cpp src/app/RecordingPlayer.cpp src/app/RecordNaming.cpp)

# ── The window (shared by the application and test_gui) ──────────────────────
add_library(esa_gui STATIC
    src/gui/MainWindow.cpp src/gui/ParamWidgets.cpp src/gui/DetailsWindow.cpp
    src/gui/AboutDialog.cpp src/gui/RadioDiscoveryDialog.cpp
    src/gui/shell/HeaderBar.cpp src/gui/shell/StatusChip.cpp
    src/gui/pages/Pages.cpp src/gui/pages/RadioPage.cpp src/gui/pages/NrPage.cpp
    src/gui/pages/OutputPage.cpp
    src/gui/pages/StageHost.cpp src/gui/pages/GatePage.cpp
    src/gui/pages/CompPage.cpp src/gui/pages/TubePage.cpp src/gui/pages/ExciterPage.cpp
    src/gui/binding/StageBinding.cpp)
target_link_libraries(esa_gui PUBLIC esa_core aether_gui Qt6::Gui Qt6::Widgets)
# The EQ page and its analyser's feed, in their own block so the line re-applies cleanly.
target_sources(esa_gui PRIVATE src/gui/pages/EqPage.cpp src/gui/binding/EqFeed.cpp)
# Final Output: this computer's output settings on its page, and the feed of its meter and scope.
target_sources(esa_gui PRIVATE src/gui/pages/StationBand.cpp src/gui/binding/OutputFeed.cpp)
# What --screenshot saves, here so test_look renders its goldens through the same code.
target_sources(esa_gui PRIVATE src/gui/Screenshot.cpp)

# ── The window's frame (ui/frame): the platform's own, or AetherRX's ─────────
# Its own block, so it re-applies onto a restructured build. A Mac hides the system's title
# text through AppKit, which Qt has no call for; Windows 11 rounds a frameless window's
# corners through the DWM.
target_sources(esa_gui PRIVATE src/gui/shell/WindowChrome.cpp)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(esa_gui PRIVATE src/gui/shell/mac/WindowChromeMac.mm)
    target_link_libraries(esa_gui PRIVATE "-framework AppKit")
elseif(WIN32)
    target_link_libraries(esa_gui PRIVATE dwmapi)
endif()
qt_add_resources(esa_gui "app_icon"
    PREFIX "/icons"
    BASE ${CMAKE_SOURCE_DIR}/packaging/icons
    FILES ${CMAKE_SOURCE_DIR}/packaging/icons/ExpertSDR3Audio-256.png)

# ── Licences ─────────────────────────────────────────────────────────────────
# The GPL, and the licences of what the app is built from or ships with, which ask that their
# text go with every copy: the Inter typeface built into it, the code built into it, and the
# libraries the packages bundle (Qt, FFTW). Each is copied into <build>/Licenses under a name
# that says whose it is, and every package carries that folder: Contents/Resources/Licenses in
# the Mac app, Licenses beside the program on Windows, and
# /usr/share/doc/expertsdr3-audio/Licenses on Linux. packaging/licenses holds the two the
# source tree has no other copy of.
set(ESA_LICENSE_DIR ${CMAKE_BINARY_DIR}/Licenses)
set(ESA_LICENSES "")
function(esa_add_license source name)
    configure_file(${source} ${ESA_LICENSE_DIR}/${name} COPYONLY)
    set(ESA_LICENSES ${ESA_LICENSES} ${ESA_LICENSE_DIR}/${name} PARENT_SCOPE)
endfunction()
esa_add_license(${CMAKE_SOURCE_DIR}/LICENSE GPL-3.0.txt)
esa_add_license(${AETHER_DIR}/THIRD_PARTY_LICENSES AetherSDR-THIRD_PARTY_LICENSES.txt)
esa_add_license(${AETHER_TP}/inter/LICENSE.txt Inter-OFL.txt)
esa_add_license(${AETHER_TP}/rnnoise/COPYING RNNoise-BSD-3-Clause.txt)
esa_add_license(${AETHER_TP}/r8brain/LICENSE r8brain-MIT.txt)
esa_add_license(${AETHER_TP}/libspecbleach/LICENSE libspecbleach-LGPL-2.1.txt)
esa_add_license(${AETHER_TP}/wdsp/LICENSE WDSP-GPL-2.0.txt)
esa_add_license(${AETHER_TP}/deepfilter/LICENSE-MIT DeepFilterNet-MIT.txt)
esa_add_license(${AETHER_TP}/deepfilter/LICENSE-APACHE DeepFilterNet-Apache-2.0.txt)
esa_add_license(${CMAKE_SOURCE_DIR}/packaging/licenses/FFTW-COPYRIGHT.txt FFTW-COPYRIGHT.txt)
esa_add_license(${CMAKE_SOURCE_DIR}/packaging/licenses/LGPL-3.0.txt Qt-LGPL-3.0.txt)
if(ESA_PORTAUDIO_LICENSE)
    esa_add_license(${ESA_PORTAUDIO_LICENSE} PortAudio-MIT.txt)
endif()
set_source_files_properties(${ESA_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Licenses)

# ── The application ──────────────────────────────────────────────────────────
set(ESA_ICON_ICNS ${CMAKE_SOURCE_DIR}/packaging/icons/ExpertSDR3Audio.icns)
set_source_files_properties(${ESA_ICON_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

qt_add_executable(ExpertSDR3Audio MACOSX_BUNDLE WIN32
    src/main.cpp
    $<$<BOOL:${APPLE}>:${ESA_ICON_ICNS}>
    $<$<BOOL:${WIN32}>:packaging/windows/app.rc>)
target_link_libraries(ExpertSDR3Audio PRIVATE esa_gui)
if(APPLE)
    target_sources(ExpertSDR3Audio PRIVATE ${ESA_LICENSES})
elseif(WIN32)
    # Beside the program, where scripts/package-windows.ps1 takes them from.
    add_custom_command(TARGET ExpertSDR3Audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${ESA_LICENSE_DIR} $<TARGET_FILE_DIR:ExpertSDR3Audio>/Licenses)
endif()
target_compile_definitions(ExpertSDR3Audio PRIVATE ESA_VERSION="${ESA_VERSION}")

# The model is embedded so one binary works everywhere; DeepFilterFilter
# extracts it to app data on first use because libdf needs a real file path.
if(ENABLE_DFNR AND EXISTS ${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz)
    # The alias must be set first: qt_add_resources reads it when called.
    set_source_files_properties(${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz
        PROPERTIES QT_RESOURCE_ALIAS "DeepFilterNet3_onnx.dfmodel")
    qt_add_resources(ExpertSDR3Audio "dfnr_model"
        PREFIX "/models"
        BASE ${DF_DIR}/models
        FILES ${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz)
endif()

set_target_properties(ExpertSDR3Audio PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "ExpertSDR3 Audio"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.vu3esv.ExpertSDR3Audio"
    MACOSX_BUNDLE_BUNDLE_VERSION ${ESA_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${ESA_VERSION}
    MACOSX_BUNDLE_ICON_FILE ExpertSDR3Audio.icns
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/Info.plist.in)
if(DFNR_DLL AND EXISTS ${DFNR_DLL})
    add_custom_command(TARGET ExpertSDR3Audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DFNR_DLL} $<TARGET_FILE_DIR:ExpertSDR3Audio>)
endif()
install(TARGETS ExpertSDR3Audio BUNDLE DESTINATION . RUNTIME DESTINATION bin)

if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/expertsdr3-audio.desktop DESTINATION share/applications)
    install(FILES packaging/icons/ExpertSDR3Audio-256.png
        DESTINATION share/icons/hicolor/256x256/apps RENAME expertsdr3-audio.png)
    install(FILES packaging/icons/ExpertSDR3Audio.svg
        DESTINATION share/icons/hicolor/scalable/apps RENAME expertsdr3-audio.svg)
    install(FILES LICENSE DESTINATION share/doc/expertsdr3-audio RENAME copyright)
    install(FILES ${ESA_LICENSES} DESTINATION share/doc/expertsdr3-audio/Licenses)

    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGE_NAME expertsdr3-audio)
    set(CPACK_PACKAGE_VERSION ${ESA_VERSION})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ExpertSDR3 receive audio through the AetherRX noise-reduction chain")
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "VU3ESV <VU3ESV@users.noreply.github.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION hamradio)
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/VU3ESV/ExpertSDR3-Audio")
    # Listed by hand rather than generated: Debian's Qt adds a strict qt6-websockets-abi
    # (= 6.4.2) dependency to anything built against it, which would stop one package
    # installing on a newer Debian, although Qt's public API stays compatible. The newer
    # names are what the 64-bit time_t transition renamed some of these to. CI installs and
    # runs the result on each release listed in build.yml, which is what keeps this honest.
    # qt6-qpa-plugins is what lets the window open at all.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libc6 (>= 2.36), libstdc++6, libasound2 | libasound2t64, libfftw3-double3, libfftw3-single3, libqt6core6 | libqt6core6t64, libqt6gui6 | libqt6gui6t64, libqt6widgets6 | libqt6widgets6t64, libqt6network6 | libqt6network6t64, libqt6websockets6 | libqt6websockets6t64, qt6-qpa-plugins")
    include(CPack)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    # Every test runs in the test sandbox (src/app/TestSandbox.h): no real audio output and none
    # of ExpertSDR3's ports. A test that forgets its fake output or its mock radio is refused
    # them, with a "Test sandbox:" warning, instead of making a sound or reaching the real
    # radio. TestSandbox.cpp turns it on in the program itself too, for runs outside ctest.
    set(esa_test_env "QT_QPA_PLATFORM=offscreen;ESA_TEST_SANDBOX=1")
    function(esa_test name)
        qt_add_executable(${name} tests/${name}.cpp tests/TestSandbox.cpp ${ARGN})
        target_link_libraries(${name} PRIVATE esa_core Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${esa_test_env}")
    endfunction()
    esa_test(test_tci_protocol)
    esa_test(test_mute_controller)
    esa_test(test_drift)
    esa_test(test_playback)
    esa_test(test_rx_chain)
    esa_test(test_engine_e2e tests/MockExpert.cpp)
    esa_test(test_discovery tests/MockExpert.cpp)
    esa_test(test_profiles)
    # Recording: the core, which re-runs itself as a child for the disk-full and crash cases, and
    # the app run headless with --record and --capture against an in-process MockExpert.
    esa_test(test_recorder tests/MockExpert.cpp)
    add_dependencies(test_recorder ExpertSDR3Audio)
    target_compile_definitions(test_recorder PRIVATE ESA_APP_BINARY="$<TARGET_FILE:ExpertSDR3Audio>")
    esa_test(test_gui tests/MockExpert.cpp)
    target_link_libraries(test_gui PRIVATE esa_gui)
    # The window's shell: rail, header, menus, geometry and every way to close it. MockExpert
    # only for REC and PLAY from the window, end to end.
    esa_test(test_shell tests/MockExpert.cpp)
    target_link_libraries(test_shell PRIVATE esa_gui)

    # The sync-safety contract: the vendored pages against the shims alone, and then in the
    # real window against a real, unconnected engine on the test thread, where every object
    # the window looks up and every correction it makes from outside is checked. MockExpert
    # only to show that BYPASS sends the radio nothing.
    esa_test(test_aether_gui tests/MockExpert.cpp)
    target_link_libraries(test_aether_gui PRIVATE esa_gui)
    # The vendored pages cache small drawn images (check boxes, arrows) in the user's cache
    # folder, which Qt's test mode only moves to ~/.qttest. Keep the GUI tests' copies in the
    # build tree. Test mode builds its path from HOME; on macOS the system asks
    # CFFIXED_USER_HOME, so both are set. Windows test runs still use %LOCALAPPDATA%\qttest.
    set(esa_test_home "${CMAKE_BINARY_DIR}/test-home")
    file(MAKE_DIRECTORY "${esa_test_home}")
    # ESA_CHECK_MIRROR: the window compares the pages' display mirror with its settings once a
    # second and logs the first difference, as a debug build always does.
    set_tests_properties(test_gui test_shell test_aether_gui PROPERTIES ENVIRONMENT
        "${esa_test_env};HOME=${esa_test_home};CFFIXED_USER_HOME=${esa_test_home};ESA_CHECK_MIRROR=1")
    # The approved look: each page as --screenshot draws it from its tests/look fixture, against
    # the goldens drawn in this same environment (tests/golden/<key>/, docs/RELEASING.md). Where
    # there are none yet it compares nothing, and exits 77 so ctest lists it as skipped.
    esa_test(test_look)
    target_link_libraries(test_look PRIVATE esa_gui)
    target_compile_definitions(test_look PRIVATE ESA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    set_tests_properties(test_look PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT
        "${esa_test_env};HOME=${esa_test_home};CFFIXED_USER_HOME=${esa_test_home};ESA_CHECK_MIRROR=1")
    # Every licence a package must carry, in the folder this platform's packaging takes them
    # from: inside the Mac app, beside the program on Windows, and on Linux the build's own,
    # which install() puts into the .deb and the AppImage.
    if(APPLE)
        set(esa_licences_at "$<TARGET_BUNDLE_CONTENT_DIR:ExpertSDR3Audio>/Resources/Licenses")
    elseif(WIN32)
        set(esa_licences_at "$<TARGET_FILE_DIR:ExpertSDR3Audio>/Licenses")
    else()
        set(esa_licences_at "${ESA_LICENSE_DIR}")
    endif()
    add_test(NAME test_licences COMMAND ${CMAKE_COMMAND} "-DDIR=${esa_licences_at}"
        "-DPORTAUDIO=$<BOOL:${ESA_PORTAUDIO_LICENSE}>" -P ${CMAKE_SOURCE_DIR}/tests/CheckLicences.cmake)
    if(ENABLE_DFNR AND EXISTS ${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz)
        qt_add_resources(test_rx_chain "dfnr_model_test"
            PREFIX "/models"
            BASE ${DF_DIR}/models
            FILES ${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz)
    endif()
endif()
