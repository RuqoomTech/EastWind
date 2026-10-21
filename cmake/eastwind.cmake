# EastWind fork specific build options.
# Kept in this file so that changes to upstream files stay minimal.

option(EW_BUILD_OPTION_LARGE_ADDRESS_AWARE "Allow 32-bit executables to use up to 4 GB of address space on 64-bit Windows." ON)
add_feature_info(EastWindLargeAddressAware EW_BUILD_OPTION_LARGE_ADDRESS_AWARE "Large address aware 32-bit executables")

# Only meaningful for 32-bit Windows executables.
if(EW_BUILD_OPTION_LARGE_ADDRESS_AWARE AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    if(MSVC)
        string(APPEND CMAKE_EXE_LINKER_FLAGS " /LARGEADDRESSAWARE")
    elseif(MINGW)
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--large-address-aware")
    endif()
endif()

# Renderer backend. DX8 is the upstream renderer and stays the default.
# DX9 is the EastWind renderer: our own files beside the DX8 ones (dx9wrapper.cpp next to
# dx8wrapper.cpp), swapped in by cmake/eastwind_dx9.cmake. Raw DX8 files are never edited.
set(EW_RENDERER "DX8" CACHE STRING "Renderer backend to build: DX8 (upstream) or DX9 (EastWind).")
set_property(CACHE EW_RENDERER PROPERTY STRINGS DX8 DX9)
if(NOT EW_RENDERER MATCHES "^(DX8|DX9)$")
    message(FATAL_ERROR "EW_RENDERER must be DX8 or DX9, got '${EW_RENDERER}'.")
endif()
if(EW_RENDERER STREQUAL "DX9")
    set(EW_RENDERER_IS_DX9 ON)
else()
    set(EW_RENDERER_IS_DX9 OFF)
endif()
add_feature_info(EastWindRendererDX9 EW_RENDERER_IS_DX9 "EastWind Direct3D 9 renderer")

if(EW_RENDERER STREQUAL "DX9")
    if(IS_VS6_BUILD)
        message(FATAL_ERROR "EW_RENDERER=DX9 needs a modern MSVC; VC6 builds only support DX8.")
    endif()
    if(RTS_BUILD_GENERALS)
        message(FATAL_ERROR "EW_RENDERER=DX9 is Zero Hour only; configure with -DRTS_BUILD_GENERALS=OFF.")
    endif()
    if(NOT MSVC OR NOT TARGET d3d8lib)
        message(FATAL_ERROR "EW_RENDERER=DX9 currently supports 32-bit Windows MSVC builds only.")
    endif()

    # Until every raw D3D8 file has a DX9 version, the DX9 build still compiles
    # some DX8 code, so it keeps the DX8 SDK. Turn this on once the copy set is complete:
    # it removes d3d8/d3dx8 so any leftover DX8 include fails loudly.
    option(EW_DX9_NO_D3D8 "Remove the DX8 SDK (d3d8, d3dx8) from the DX9 build." OFF)
    add_feature_info(EastWindDX9NoD3D8 EW_DX9_NO_D3D8 "DX9 build without the DX8 SDK")

    # d3d8lib is the renderer dependency target every engine library links.
    # It is rewired here instead of editing the upstream CMakeLists files.
    if(EW_DX9_NO_D3D8)
        # Remove only d3d8 and d3dx8; keep the rest (dinput8, dxguid, legacy stdio, link options).
        get_target_property(_ew_d3d_libs d3d8lib INTERFACE_LINK_LIBRARIES)
        list(REMOVE_ITEM _ew_d3d_libs d3d8 d3dx8 d3dx8d)
        list(APPEND _ew_d3d_libs d3d9)
        set_property(TARGET d3d8lib PROPERTY INTERFACE_LINK_LIBRARIES "${_ew_d3d_libs}")
        # The DX8 SDK folder holds d3d8.h and d3dx8*.h; d3d9.h, dinput8 and dxguid come from the Windows SDK.
        set_property(TARGET d3d8lib PROPERTY INTERFACE_LINK_DIRECTORIES "")
        set_property(TARGET d3d8lib PROPERTY INTERFACE_INCLUDE_DIRECTORIES "")
        set_property(TARGET d3d8lib PROPERTY INTERFACE_COMPILE_DEFINITIONS "")
        unset(_ew_d3d_libs)
    else()
        target_link_libraries(d3d8lib INTERFACE d3d9)
    endif()
    # Global, so every file sees it: shared files use it to include the DX9 headers.
    add_compile_definitions(EW_RENDERER_DX9)

    include(cmake/eastwind_dx9.cmake)
endif()
