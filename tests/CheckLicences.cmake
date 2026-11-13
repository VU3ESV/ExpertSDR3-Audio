# What every package must carry, by the names CMakeLists.txt gives them: the GPL, and the
# licences of what the app is built from or ships with, which ask that their text go with every
# copy. ctest runs this as test_licences, with DIR the folder this platform's packaging takes
# them from, and PORTAUDIO true where PortAudio is built into the app.
#
#   cmake -DDIR=<folder> -DPORTAUDIO=<0|1> -P tests/CheckLicences.cmake

set(required
    GPL-3.0.txt
    AetherSDR-THIRD_PARTY_LICENSES.txt
    Inter-OFL.txt
    RNNoise-BSD-3-Clause.txt
    r8brain-MIT.txt
    libspecbleach-LGPL-2.1.txt
    WDSP-GPL-2.0.txt
    DeepFilterNet-MIT.txt
    DeepFilterNet-Apache-2.0.txt
    FFTW-COPYRIGHT.txt
    Qt-LGPL-3.0.txt)
if(PORTAUDIO)
    list(APPEND required PortAudio-MIT.txt)
endif()

set(missing "")
foreach(name IN LISTS required)
    set(path "${DIR}/${name}")
    # An empty file is as good as none.
    if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
        file(SIZE "${path}" size)
    else()
        set(size 0)
    endif()
    if(size EQUAL 0)
        list(APPEND missing ${name})
    endif()
endforeach()

if(missing)
    list(JOIN missing ", " missing)
    message(FATAL_ERROR "Missing from ${DIR}, so the packages would go out without them: ${missing}")
endif()
list(LENGTH required count)
message(STATUS "All ${count} licences are in ${DIR}")
