add_subdirectory(Precompiled)

add_library(depsi_always INTERFACE)
target_link_libraries(depsi_always INTERFACE
    deps_precompiled # Gives force included headers. Link early.
    deps_config
)

add_subdirectory(Utility)
add_subdirectory(Bink)
add_subdirectory(Miles)

if (WIN32)
    add_subdirectory(DbgHelp)
    add_subdirectory(Usp10)
else()
    add_library(deps_dbghelp INTERFACE)
    add_library(deps_usp10 INTERFACE)
endif()
