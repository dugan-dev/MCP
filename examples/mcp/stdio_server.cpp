#include "server_setup.h"
#include <iostream>
#include <mcp/stdio.h>
int main() {
    try {
        mcp::Server server;
        register_examples(server);
        mcp::run_stdio(server, std::cin, std::cout);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
