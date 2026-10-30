# EastWind fork specific build options.
# Kept in this file so that changes to upstream files stay minimal.

option(RTS_BUILD_OPTION_LARGE_ADDRESS_AWARE "Allow 32-bit executables to use up to 4 GB of address space on 64-bit Windows." ON)
add_feature_info(EastWindLargeAddressAware RTS_BUILD_OPTION_LARGE_ADDRESS_AWARE "Large address aware 32-bit executables")

# Only meaningful for 32-bit Windows executables.
if(RTS_BUILD_OPTION_LARGE_ADDRESS_AWARE AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    if(MSVC)
        string(APPEND CMAKE_EXE_LINKER_FLAGS " /LARGEADDRESSAWARE")
    elseif(MINGW)
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--large-address-aware")
    endif()
endif()

# The renderer is Direct3D 9 (D3D9Ex) with HLSL shaders compiled by fxc; it needs MSVC.
if(NOT MSVC)
    message(FATAL_ERROR "EastWind builds with MSVC only.")
endif()

include(cmake/eastwind_shaders.cmake)
