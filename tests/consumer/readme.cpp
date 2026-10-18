#include <mcp/all.h>
#include <iostream>
int main() {
    mcp::Server server;
    auto result = server.dispatch(mcp::make_request(1, "server/discover"));
    std::cout << mcp::serialize_json(result) << '\n';
}
