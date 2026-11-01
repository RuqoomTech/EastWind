add_library(core_browserdispatch INTERFACE)

if(WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows")
    # Use midl for MSVC builds
    add_custom_command(
        OUTPUT BrowserDispatch_i.c BrowserDispatch.h
        COMMAND midl.exe "${CMAKE_CURRENT_LIST_DIR}\\BrowserDispatch.idl" /header BrowserDispatch.h /iid BrowserDispatch_i.c
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/BrowserDispatch.idl"
        VERBATIM
    )
    add_library(core_browserdispatchwin STATIC BrowserDispatch_i.c)

    if(TARGET core_browserdispatchwin)
        set_target_properties(core_browserdispatchwin PROPERTIES OUTPUT_NAME browserdispatchwin)
        target_link_libraries(core_browserdispatch INTERFACE core_browserdispatchwin)
    endif()
endif()

target_include_directories(core_browserdispatch INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/..)
