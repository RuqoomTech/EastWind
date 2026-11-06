# w3x2w3d: converts RA3 W3X art to Zero Hour W3D files (WW3D2 w3xconvert.cpp does the work).
add_executable(z_w3x2w3d)
set_target_properties(z_w3x2w3d PROPERTIES
    OUTPUT_NAME "w3x2w3d${RTS_BUILD_OUTPUT_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/GeneralsMD"
)

target_sources(z_w3x2w3d PRIVATE w3x2w3d.cpp)

target_link_libraries(z_w3x2w3d PRIVATE
    core_wwstub # avoid linking GameEngine
    imm32
    vfw32
    winmm
    z_wwvegas
    zi_always
)
