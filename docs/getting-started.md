# Getting started

Build and install using the [build guide](build.md). A native application includes `<mcp/all.h>` and links `MCP::MCP`.

## Expose a local tool

The shipped [`mcp_stdio_server`](https://github.com/dugan-dev/MCP/blob/main/examples/stdio_server.cpp) registers echo/add tools, a resource and a prompt. Its registration code is in [`server_setup.h`](https://github.com/dugan-dev/MCP/blob/main/examples/server_setup.h). A minimal echo server follows the same pattern:

```cpp
#include <mcp/all.h>
#include <iostream>

int main() {
  mcp::Server server;
  server.add_tool({"echo", "Return a message", {{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}, {"required", {"message"}}}},
                  [](const mcp::Json& args, mcp::RequestContext&) { return mcp::text_result(mcp::text(args["message"])); });
  mcp::run_stdio(server, std::cin, std::cout);
}
```

Keep stdout exclusively for protocol messages. Write diagnostics to stderr. Handlers can run concurrently; protect shared application state.

## Connect a client

```cpp
mcp::ProcessOptions process;
process.executable = "/absolute/path/to/server";
auto transport = std::make_shared<mcp::StdioTransport>(process);
mcp::Client client(transport);
auto capabilities = client.discover();
auto result = client.call_tool("echo", {{"message", "Hello, MCP!"}});
client.close();
```

Use an absolute executable path. Configure `arguments`, `working_directory`, `environment` and `inherit_environment` explicitly where needed. Strings are UTF-8; embedded NULs are rejected. `close()` wakes pending operations and cleans up the owned subprocess tree. The [complete client example](https://github.com/dugan-dev/MCP/blob/main/examples/stdio_client.cpp) includes error handling.

For HTTP, construct `HttpTransport` from `HttpClientOptions` with its `url`; the `Client` API is unchanged. [`http_server.cpp`](https://github.com/dugan-dev/MCP/blob/main/examples/http_server.cpp) and [`http_client.cpp`](https://github.com/dugan-dev/MCP/blob/main/examples/http_client.cpp) provide runnable examples. `HttpServer` borrows its `Server`; stop the HTTP server before destroying the server. TLS and authentication are deployment choices, not automatically established by a loopback example.

## Use the CLI

```sh
mcp --help
mcp --version
mcp --exec /absolute/path/to/mcp_stdio_server tools list
mcp --url http://127.0.0.1:8080/mcp tools list
```

`mcp serve --config <path>` starts a configurable server. `mcp --help` documents request, resource, prompt and tool commands. Shells differ in JSON quoting; prefer argument arrays when invoking the CLI from a program. The [CLI integration test](https://github.com/dugan-dev/MCP/blob/main/tests/mcp/standalone_cli.py) demonstrates real calls without shell quoting ambiguity.

## Set a budget

```cpp
std::stop_source stop;
mcp::RequestOptions options;
options.timeout = std::chrono::seconds(5);
options.stop = stop.get_token();
auto result = client.call_tool("echo", {{"message", "Hello"}}, options);
```

Another thread can request cancellation through `stop.request_stop()`. Server handlers should call `context.throw_if_cancelled()` during lengthy work. Timeouts do not roll back tool side effects or forcibly stop arbitrary application callback code.

Read the [API contract](api-reference.md) before using custom memory resources, raw extension handlers or authentication callbacks.
