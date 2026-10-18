#include <iostream>
#include <mcp/client.h>
#include <mcp/http.h>
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: mcp_http_client http://127.0.0.1:PORT/mcp\n";
        return 1;
    }
    try {
        auto transport = std::make_shared<mcp::HttpTransport>(mcp::HttpClientOptions{argv[1]});
        mcp::Client client(transport);
        std::cout << mcp::serialize_json(client.discover()) << '\n';
        std::cout << mcp::serialize_json(client.list_tools()) << '\n';
        std::cout << mcp::serialize_json(client.call_tool("echo", {{"message", "Hello, MCP!"}}))
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
