#include "server_setup.h"
#include <iostream>
#include <mcp/http.h>
int main(int argc, char **argv) {
    try {
        mcp::Server server;
        register_examples(server);
        mcp::HttpServerOptions options;
        if (argc > 1)
            options.port = static_cast<std::uint16_t>(std::stoul(argv[1]));
        if (argc > 3) {
            options.certificate_file = argv[2];
            options.private_key_file = argv[3];
        }
        mcp::HttpServer listener(server, options);
        listener.start();
        std::cout << listener.port() << std::endl;
        std::string line;
        std::getline(std::cin, line);
        listener.stop();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
