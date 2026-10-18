# Include this fragment after MCP::MCP and enable_testing() are available.
add_executable(test_stress stress/stress.cpp)
set_target_properties(test_stress PROPERTIES OUTPUT_NAME mcp_stress FOLDER "Tests")
target_link_libraries(test_stress PRIVATE MCP::MCP Threads::Threads)
add_test(NAME mcp_stress_smoke COMMAND test_stress 1297047635 2000 4)
set_tests_properties(mcp_stress_smoke PROPERTIES TIMEOUT 240)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(MCP_REAL_USAGE_CLI)
    if(TARGET lib_cli_impl)
        list(APPEND MCP_REAL_USAGE_CLI --cli "$<TARGET_FILE:exe_cli>")
    endif()
    add_test(NAME mcp_real_usage
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/stress/real_usage.py"
            --build "$<TARGET_FILE_DIR:example_stdio_server>" --calls 1000 --workers 16
            ${MCP_REAL_USAGE_CLI})
    set_tests_properties(mcp_real_usage PROPERTIES TIMEOUT 180 LABELS "stress;process;e2e")
    foreach(transport IN ITEMS stdio http)
        add_test(NAME mcp_process_fanout_${transport}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/stress/process_fanout.py"
                --build "$<TARGET_FILE_DIR:example_stdio_client>" --clients 8 --transport ${transport})
        set_tests_properties(mcp_process_fanout_${transport} PROPERTIES TIMEOUT 120 LABELS "stress;process")
    endforeach()
endif()
