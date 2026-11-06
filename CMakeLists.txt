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

# ── Application core (everything except the GUI, shared with tests) ──────────
add_library(esa_core STATIC
    src/tci/TciProtocol.cpp src/tci/TciClient.cpp src/tci/MuteController.cpp
    src/dsp/RxChain.cpp src/dsp/NrRegistry.cpp
    src/audio/PlaybackBuffer.cpp src/audio/PortAudioSink.cpp
    src/app/Engine.cpp src/app/Settings.cpp src/app/WavWriter.cpp)
target_include_directories(esa_core PUBLIC src)
target_link_libraries(esa_core PUBLIC
    Qt6::Core Qt6::Network Qt6::WebSockets aether_dsp esa_portaudio)

# ── The application ──────────────────────────────────────────────────────────
set(ESA_ICON_ICNS ${CMAKE_SOURCE_DIR}/packaging/icons/ExpertSDR3Audio.icns)
set_source_files_properties(${ESA_ICON_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

qt_add_executable(ExpertSDR3Audio MACOSX_BUNDLE WIN32
    src/main.cpp src/gui/MainWindow.cpp src/gui/ParamWidgets.cpp
    $<$<BOOL:${APPLE}>:${ESA_ICON_ICNS}>
    $<$<BOOL:${WIN32}>:packaging/windows/app.rc>)
target_link_libraries(ExpertSDR3Audio PRIVATE esa_core Qt6::Gui Qt6::Widgets)
target_compile_definitions(ExpertSDR3Audio PRIVATE ESA_VERSION="${ESA_VERSION}")

qt_add_resources(ExpertSDR3Audio "app_icon"
    PREFIX "/icons"
    BASE ${CMAKE_SOURCE_DIR}/packaging/icons
    FILES ${CMAKE_SOURCE_DIR}/packaging/icons/ExpertSDR3Audio-256.png)

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

    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGE_NAME expertsdr3-audio)
    set(CPACK_PACKAGE_VERSION ${ESA_VERSION})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ExpertSDR3 receive audio through the AetherRX noise-reduction chain")
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "VU3ESV <VU3ESV@users.noreply.github.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION hamradio)
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/VU3ESV/ExpertSDR3-Audio")
    # Works out Qt, PortAudio and FFTW from what the binary really links.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # A plugin, so that scan cannot see it. Without it the window cannot open at all.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS qt6-qpa-plugins)
    include(CPack)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    function(esa_test name)
        qt_add_executable(${name} tests/${name}.cpp ${ARGN})
        target_link_libraries(${name} PRIVATE esa_core Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endfunction()
    esa_test(test_tci_protocol)
    esa_test(test_mute_controller)
    esa_test(test_drift)
    esa_test(test_playback)
    esa_test(test_rx_chain)
    esa_test(test_engine_e2e tests/MockExpert.cpp)
    esa_test(test_gui src/gui/MainWindow.cpp src/gui/ParamWidgets.cpp)
    target_link_libraries(test_gui PRIVATE Qt6::Gui Qt6::Widgets)
    if(ENABLE_DFNR AND EXISTS ${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz)
        qt_add_resources(test_rx_chain "dfnr_model_test"
            PREFIX "/models"
            BASE ${DF_DIR}/models
            FILES ${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz)
    endif()
endif()
