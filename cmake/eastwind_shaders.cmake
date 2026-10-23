# EastWind DX9 renderer: HLSL shaders compiled with fxc at build time.
#
# Sources are EastWind/Shaders/*.hlsl. Each entry below compiles to
# ${CMAKE_BINARY_DIR}/ew_shaders/<name>.h, a BYTE array g_ew_<name> that ewshaders_dx9.cpp
# embeds in the exe, so the game needs no shader files and no D3DCompiler/D3DX at runtime.
# The files that include these headers get the ew_shaders dependency and include folder
# from cmake/eastwind_dx9.cmake (see EastWind/dx9_new_files.txt).

set(EW_SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/EastWind/Shaders")
set(EW_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/ew_shaders")

# fxc ships with the Windows SDK. The vcvars environment puts it on PATH; otherwise take the newest SDK.
file(GLOB _ew_fxc_dirs LIST_DIRECTORIES true "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64")
list(SORT _ew_fxc_dirs COMPARE NATURAL ORDER DESCENDING)
find_program(EW_FXC_EXECUTABLE NAMES fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" ${_ew_fxc_dirs}
    DOC "fxc.exe from the Windows SDK, used to compile the EastWind HLSL shaders")
unset(_ew_fxc_dirs)
if(NOT EW_FXC_EXECUTABLE)
    message(FATAL_ERROR "EW_RENDERER=DX9 needs fxc.exe from the Windows 10/11 SDK. Set EW_FXC_EXECUTABLE to its path.")
endif()

option(EW_SHADER_DEBUG "Compile the EastWind shaders without optimisation and with debug info." OFF)
if(EW_SHADER_DEBUG)
    set(_ew_fxc_flags /Zi /Od)
else()
    set(_ew_fxc_flags /O3)
endif()

# <name> <source file> <entry point> <profile>
# M3.0 parity ports use the 2_0 profiles: they run beside fixed-function vertex processing and
# fixed-function fog, which vs_3_0/ps_3_0 cannot mix with. New M3.1+ shaders use 3_0.
set(EW_SHADERS
    trees_vs                trees.hlsl       TreesVS              vs_2_0
    wave_vs                 wave.hlsl        WaveVS               vs_2_0
    wave_ps                 wave.hlsl        WavePS               ps_2_0
    river_ps                water.hlsl       RiverPS              ps_2_0
    trapezoid_ps            water.hlsl       TrapezoidPS          ps_2_0
    terrain_ps              terrain.hlsl     TerrainPS            ps_2_0
    terrain_noise1_ps       terrain.hlsl     TerrainNoise1PS      ps_2_0
    terrain_noise2_ps       terrain.hlsl     TerrainNoise2PS      ps_2_0
    road_noise2_ps          terrain.hlsl     RoadNoise2PS         ps_2_0
    flat_terrain_ps         terrain.hlsl     FlatTerrainPS        ps_2_0
    flat_terrain0_ps        terrain.hlsl     FlatTerrain0PS       ps_2_0
    flat_terrain_noise1_ps  terrain.hlsl     FlatTerrainNoise1PS  ps_2_0
    flat_terrain_noise2_ps  terrain.hlsl     FlatTerrainNoise2PS  ps_2_0
    monochrome_ps           monochrome.hlsl  MonochromePS         ps_2_0
    profiler_swizzle_ps     profiler.hlsl    ProfilerSwizzlePS    ps_2_0
    mesh_n_t0_vs            mesh.hlsl        MeshVS_N_T0          vs_3_0
    mesh_nd_t0_vs           mesh.hlsl        MeshVS_ND_T0         vs_3_0
    mesh_ns_t0_vs           mesh.hlsl        MeshVS_NS_T0         vs_3_0
    mesh_nds_t0_vs          mesh.hlsl        MeshVS_NDS_T0        vs_3_0
    mesh_n_t1_vs            mesh.hlsl        MeshVS_N_T1          vs_3_0
    mesh_nd_t1_vs           mesh.hlsl        MeshVS_ND_T1         vs_3_0
    mesh_ns_t1_vs           mesh.hlsl        MeshVS_NS_T1         vs_3_0
    mesh_nds_t1_vs          mesh.hlsl        MeshVS_NDS_T1        vs_3_0
    mesh_n_t2_vs            mesh.hlsl        MeshVS_N_T2          vs_3_0
    mesh_nd_t2_vs           mesh.hlsl        MeshVS_ND_T2         vs_3_0
    mesh_ns_t2_vs           mesh.hlsl        MeshVS_NS_T2         vs_3_0
    mesh_nds_t2_vs          mesh.hlsl        MeshVS_NDS_T2        vs_3_0
    mesh_ps                 mesh.hlsl        MeshPS               ps_3_0
    mesh_material_ps        mesh.hlsl        MeshMaterialPS       ps_3_0
    terrain_lit_vs          terrain_lit.hlsl TerrainLitVS         vs_3_0
    terrain_lit_ps          terrain_lit.hlsl TerrainLitPS         ps_3_0
    terrain_lit_noise1_ps   terrain_lit.hlsl TerrainLitNoise1PS   ps_3_0
    terrain_lit_noise2_ps   terrain_lit.hlsl TerrainLitNoise2PS   ps_3_0
    road_lit_vs             terrain_lit.hlsl RoadLitVS            vs_3_0
    road_lit_ps             terrain_lit.hlsl RoadLitPS            ps_3_0
    road_lit_noise1_ps      terrain_lit.hlsl RoadLitNoise1PS      ps_3_0
    road_lit_noise2_ps      terrain_lit.hlsl RoadLitNoise2PS      ps_3_0
    trees_lit_vs            trees.hlsl       TreesLitVS           vs_3_0
    trees_lit_ps            trees.hlsl       TreesLitPS           ps_3_0
)

file(GLOB _ew_shader_includes CONFIGURE_DEPENDS "${EW_SHADER_SOURCE_DIR}/*.h")
file(GLOB _ew_shader_sources CONFIGURE_DEPENDS "${EW_SHADER_SOURCE_DIR}/*.hlsl")
set(_ew_shader_outputs "")
list(LENGTH EW_SHADERS _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE 0 ${_last} 4)
    math(EXPR _i1 "${_i} + 1")
    math(EXPR _i2 "${_i} + 2")
    math(EXPR _i3 "${_i} + 3")
    list(GET EW_SHADERS ${_i} _name)
    list(GET EW_SHADERS ${_i1} _file)
    list(GET EW_SHADERS ${_i2} _entry)
    list(GET EW_SHADERS ${_i3} _profile)
    set(_src "${EW_SHADER_SOURCE_DIR}/${_file}")
    set(_out "${EW_SHADER_OUTPUT_DIR}/${_name}.h")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${EW_FXC_EXECUTABLE}" /nologo /T ${_profile} /E ${_entry} /WX ${_ew_fxc_flags}
                /Vn g_ew_${_name} /Fh "${_out}" "${_src}"
        DEPENDS "${_src}" ${_ew_shader_includes}
        COMMENT "fxc ${_file}:${_entry} (${_profile})"
        VERBATIM
    )
    list(APPEND _ew_shader_outputs "${_out}")
endforeach()

# SOURCES only lists the files in IDEs.
add_custom_target(ew_shaders DEPENDS ${_ew_shader_outputs} SOURCES ${_ew_shader_sources} ${_ew_shader_includes})
unset(_ew_shader_outputs)
unset(_ew_shader_includes)
unset(_ew_shader_sources)
unset(_ew_fxc_flags)

# ew_constants.h holds the register numbers for both the HLSL and the C++ side.
# Directory scope, so every target added after this (all of them) can include it.
include_directories("${EW_SHADER_SOURCE_DIR}")
