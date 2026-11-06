# EastWind fork specific build options.
# Kept in this file so that changes to upstream files stay minimal.

# The renderer is Direct3D 9 (D3D9Ex) with HLSL shaders compiled by fxc; it needs MSVC.
if(NOT MSVC)
    message(FATAL_ERROR "EastWind builds with MSVC only.")
endif()

# The mod needs more than 4 GB of memory, so there is no 32-bit build.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EastWind builds for x64 only. Use the win64 preset.")
endif()

# GameMath instead of the CRT math, so the game logic computes the same bits on every machine and
# compiler and replays stay in sync. Breaks CRC compatibility with retail (already lost with VS2022).
# Directory wide, so every target sees the same inline math (one-definition rule).
add_compile_definitions(FORCE_DETERMINISTIC_MATH=1)

include(cmake/eastwind_shaders.cmake)

# RA3 W3X art is XML; WW3D2 w3xconvert.cpp parses it with pugixml.
find_package(pugixml CONFIG REQUIRED)

if(RTS_BUILD_ZEROHOUR_TOOLS)
    add_subdirectory(EastWind/Tools/w3x2w3d)
endif()
