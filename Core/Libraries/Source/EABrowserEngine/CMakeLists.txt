add_library(core_browserengine INTERFACE)

if(WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows")
    # Use midl for MSVC builds
    add_custom_command(
        OUTPUT BrowserEngine_i.c BrowserEngine.h BrowserEngine.tlb
        COMMAND midl.exe "${CMAKE_CURRENT_LIST_DIR}\\BrowserEngine.idl" /header BrowserEngine.h /iid BrowserEngine_i.c /tlb BrowserEngine.tlb
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/BrowserEngine.idl"
        VERBATIM
    )
    add_library(core_browserenginewin STATIC BrowserEngine_i.c BrowserEngine.h BrowserEngine.tlb)

    if(TARGET core_browserenginewin)
        set_target_properties(core_browserenginewin PROPERTIES OUTPUT_NAME browserenginewin)
        target_link_libraries(core_browserengine INTERFACE core_browserenginewin)
    endif()
endif()

target_include_directories(core_browserengine INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/..)
