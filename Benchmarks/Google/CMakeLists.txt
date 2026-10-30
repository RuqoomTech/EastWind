add_subdirectory(Dependencies)
add_subdirectory(Core)

if(RTS_BUILD_ZEROHOUR)
    add_executable(z_googlebenchmark)
    target_link_libraries(z_googlebenchmark PRIVATE
        corei_googlebenchmark
        z_gameenginedevice
        zi_always
    )
endif()

