cmake_minimum_required(VERSION 3.25)
project(MCPInstalledConsumer LANGUAGES CXX)
find_package(MCP 0.0.1 CONFIG REQUIRED)
add_executable(mcp_consumer main.cpp)
target_link_libraries(mcp_consumer PRIVATE MCP::MCP)

add_executable(mcp_readme readme.cpp)
target_link_libraries(mcp_readme PRIVATE MCP::MCP)
