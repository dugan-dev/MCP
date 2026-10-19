#include <mcp/all.h>
#include <iostream>

int main() {
  mcp::Server server;
  server.add_tool({"hello", "Say hello"}, [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_result("Hello, MCP!"); });
  mcp::run_stdio(server, std::cin, std::cout);
}
