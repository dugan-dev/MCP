#include <iostream>
#include <mcp/client.h>
#include <mcp/stdio.h>
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: mcp_stdio_client /absolute/path/to/server [arguments...]\n";
    return 1;
  }
  try {
    mcp::ProcessOptions process;
    process.executable = argv[1];
    for (int i = 2; i < argc; ++i)
      process.arguments.emplace_back(argv[i]);
    auto transport = std::make_shared<mcp::StdioTransport>(process);
    mcp::Client client(transport);
    std::cout << mcp::serialize_json(client.discover()) << '\n';
    std::cout << mcp::serialize_json(client.call_tool("echo", {{"message", "Hello, MCP!"}})) << '\n';
    client.close();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
