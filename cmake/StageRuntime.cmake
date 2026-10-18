# Called in each owning directory: CMake post-build commands are directory scoped.
include("${CMAKE_CURRENT_LIST_DIR}/RuntimePaths.cmake")
function(mcp_stage_runtime)
    if(NOT WIN32)
        return()
    endif()
    mcp_runtime_search_directories(runtime_directories)
    get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(kind ${target} TYPE)
        get_target_property(staged ${target} MCP_RUNTIME_STAGED)
        if(kind STREQUAL "EXECUTABLE" AND NOT staged)
            set_property(TARGET ${target} PROPERTY MCP_RUNTIME_STAGED TRUE)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    "-DMCP_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:${target}>,|>"
                    "-DMCP_RUNTIME_DESTINATION=$<TARGET_FILE_DIR:${target}>"
                    "-DMCP_EXECUTABLE=$<TARGET_FILE:${target}>"
                    "-DMCP_VCPKG_EXECUTABLE=${Z_VCPKG_ROOT_DIR}/vcpkg.exe"
                    "-DMCP_DEPENDENCY_BINS=$<JOIN:${runtime_directories},|>"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CopyRuntime.cmake"
                VERBATIM)
        endif()
    endforeach()
endfunction()
