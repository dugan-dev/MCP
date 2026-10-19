add_executable(mcp_benchmark main.cpp)
target_link_libraries(mcp_benchmark PRIVATE MCP::MCP)
file(READ "${PROJECT_SOURCE_DIR}/vcpkg.json" _mcp_benchmark_manifest)
string(JSON _mcp_benchmark_baseline GET "${_mcp_benchmark_manifest}" builtin-baseline)
target_compile_definitions(mcp_benchmark PRIVATE
    MCP_BENCH_BACKEND="${UNIMEMORY_BACKEND}"
    MCP_BENCH_STATISTICS="$<IF:$<BOOL:${UNIMEMORY_STATISTICS}>,ON,OFF>"
    MCP_BENCH_CONFIGURATION="$<CONFIG>"
    MCP_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    MCP_BENCH_SYSTEM="${CMAKE_SYSTEM_NAME}"
    MCP_BENCH_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
    MCP_BENCH_CHECKS="${UNIMEMORY_CHECKS}"
    MCP_BENCH_LINKAGE="$<IF:$<BOOL:${BUILD_SHARED_LIBS}>,shared,static>"
    MCP_BENCH_DEPENDENCIES="${_mcp_benchmark_baseline}")
if(MSVC)
    target_compile_options(mcp_benchmark PRIVATE /utf-8)
endif()
mcp_stage_runtime()
