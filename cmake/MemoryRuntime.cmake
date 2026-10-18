# Preserve the caller's hook and its value while scheduling our directory-owned
# post-build commands after all vendored memory targets have been declared.
set(CMAKE_PROJECT_UniMemory_INCLUDE "${_mcp_memory_previous_project_include}")
foreach(hook IN LISTS _mcp_memory_previous_project_include)
    include("${hook}")
endforeach()
cmake_language(DEFER CALL mcp_stage_runtime)
