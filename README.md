# MCP

[![Build](https://github.com/dugan-dev/MCP/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dugan-dev/MCP/actions/workflows/build.yml?query=branch%3Amain) [![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)](https://github.com/dugan-dev/MCP/releases) [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE) [![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/std/the-standard) [![CMake](https://img.shields.io/badge/CMake-3.25%2B-green.svg)](https://cmake.org/) [![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#platforms)

A modern C++20 client and server library for the Model Context Protocol, with stdio and HTTP transports.

**English** · [简体中文](README.zh-CN.md)

<details>
<summary>Contents</summary>

- [Features](#features)
- [Quick Start](#quick-start)
- [Platforms](#platforms) · [Capabilities](#capabilities)
- [Performance](#performance)
- [Build and Install](#build-and-install) · [Integration](#integration)
- [Documentation](#documentation) · [License](#license)

</details>

## Features

- **🚀 Modern C++20**: Typed client, server and transport interfaces.
- **🔧 Tools**: Register handlers and validate JSON input and output schemas.
- **📚 Resources and prompts**: Expose content and reusable prompt templates.
- **🔄 Unified transports**: Local subprocesses, HTTP/TLS and SSE notifications.
- **⏱️ Request control**: Timeouts, cancellation and configurable limits.
- **🧩 Extensions**: Authorization helpers, input workflows, tasks and skills.
- **🧠 Unified memory**: UniMemory-backed JSON allocation.
- **📦 Simple integration**: One include, `<mcp/all.h>`, and one target, `MCP::MCP`.

Version **1.0.0** implements protocol **2026-07-28**. Legacy `initialize` sessions are not supported. [Compatibility →](docs/compatibility.md)

## Quick Start

### Basic Usage

Save as `main.cpp`. This server exposes a tool named `hello`:

```cpp
#include <mcp/all.h>
#include <iostream>

int main() {
  mcp::Server server;
  server.add_tool({"hello", "Say hello"}, [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_result("Hello, MCP!"); });
  mcp::run_stdio(server, std::cin, std::cout);
}
```

Stdout carries protocol messages; write logs to stderr. Add the following tool, resource and prompt registrations before `run_stdio`.

### Tools

Declare an input schema and return a text result:

```cpp
server.add_tool({"echo", "Return a message", {{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}, {"required", {"message"}}}},
                [](const mcp::Json& args, mcp::RequestContext&) { return mcp::text_result(mcp::text(args["message"])); });
```

### Resources

Expose content through a URI:

```cpp
server.add_resource({"example://hello", "hello", "Greeting", "text/plain"},
                    [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_resource("example://hello", "Hello, MCP!"); });
```

### Prompts

Create a reusable prompt with a required argument:

```cpp
server.add_prompt({"greet", "Generate a greeting", mcp::Json::array({{{"name", "name"}, {"required", true}}})},
                  [](const mcp::Json& args, mcp::RequestContext&) { return mcp::prompt_result("Hello, " + std::string(mcp::text(args["name"]))); });
```

### Client

In a separate program, include `<mcp/all.h>` and connect to the compiled server:

```cpp
mcp::ProcessOptions process;
process.executable = "/absolute/path/to/hello";

auto transport = std::make_shared<mcp::StdioTransport>(process);
mcp::Client client(transport);

auto info = client.discover();
auto tools = client.list_tools();
auto greeting = client.call_tool("hello", mcp::Json::object());
```

If you added the registrations above, you can also call, read and retrieve them:

```cpp
auto echoed = client.call_tool("echo", {{"message", "Hello"}});
auto resource = client.read_resource("example://hello");
auto prompt = client.get_prompt("greet", {{"name", "Ada"}});
```

Call `client.close()` when finished to stop the owned subprocess.

### HTTP Server

In the basic server, replace `run_stdio` with this block. Keep `server` alive until the listener stops:

```cpp
mcp::HttpServerOptions options;
options.port = 8080;

mcp::HttpServer listener(server, options);
listener.start();
std::cin.get();  // Press Enter to stop.
listener.stop();
```

### HTTP Client

Use an HTTP transport instead of a subprocess. The client API stays the same:

```cpp
auto transport = std::make_shared<mcp::HttpTransport>(mcp::HttpClientOptions{"http://127.0.0.1:8080/mcp"});
mcp::Client client(transport);

auto info = client.discover();
auto greeting = client.call_tool("hello", mcp::Json::object());
```

### Request Options

Set a timeout and cancellation token on an open client:

```cpp
std::stop_source stop;
mcp::RequestOptions request;
request.timeout = std::chrono::seconds(5);
request.stop = stop.get_token();

auto result = client.call_tool("hello", mcp::Json::object(), request);
```

Another thread can call `stop.request_stop()`. Long-running tool handlers should check `context.throw_if_cancelled()`.

### CLI

After installation, add `build/install/release/bin` to `PATH`:

```sh
mcp --exec /absolute/path/to/hello tools list
mcp --exec /absolute/path/to/hello tools call hello
mcp --url http://127.0.0.1:8080/mcp tools list
mcp --help
```

`tools call hello` returns `Hello, MCP!`. On Windows, use the path to `hello.exe`.

## Platforms

| Platform | Compiler | Validation |
| --- | --- | --- |
| Windows x64 | MSVC | Local Debug and Release |
| Linux x64 | GCC / Clang | Local GCC Release |
| macOS | Apple Clang (Xcode 26+) | CI matrix; see the Build badge |
| Android / iOS | — | Not device-validated |

CI covers static/shared libraries and Debug/Release. [Executed checks →](docs/validation.md)

## Capabilities

| Capability | Stdio | HTTP |
| --- | :---: | :---: |
| Tools, resources, prompts | ✔ | ✔ |
| Concurrent requests and notifications | ✔ | ✔ |
| Timeouts and cancellation | ✔ | ✔ |
| Owned subprocess cleanup | ✔ | — |
| TLS and HTTP authorization | — | ✔ |

Optional extensions require matching peer support; applications supply tool logic and identity policy.

## Performance

HTTP connection reuse, concurrent requests and bounded buffers, with UniMemory-backed allocation. See [performance measurements](docs/performance.md) and [validation](docs/validation.md). Cross-SDK benchmark results have not been published.

## Build and Install

Requires **C++20**, **CMake 3.25+** and **vcpkg**. Set `VCPKG_ROOT` to your vcpkg checkout, then run from the source directory:

```sh
git clone --recurse-submodules https://github.com/dugan-dev/MCP.git
cd MCP
cmake --preset release
cmake --build --preset release
cmake --install build/MCP-release --config Release
```

Installed to `build/install/release`. Use `debug` / `Debug` for a debug build. The **`exe_cli`** target builds the **`mcp`** command-line program.

## Integration

Link MCP in your project's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(HelloMCP LANGUAGES CXX)

find_package(MCP 1.0.0 CONFIG REQUIRED)
add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE MCP::MCP)
```

Set `CMAKE_PREFIX_PATH` to the MCP installation and use the matching dependency toolchain. See the [build guide](docs/build.md) for configuration and runtime deployment.

## Documentation

- **Getting started:** [Build and integrate](docs/getting-started.md) · [Examples](https://github.com/dugan-dev/MCP/tree/main/examples)
- **Configuration:** [Build guide](docs/build.md) · [Dependencies](docs/dependencies.md)
- **Interfaces:** [API contract](docs/api-reference.md) · [Feature examples](docs/features.md)
- **Design:** [Architecture and modules](docs/architecture.md)
- **Compatibility:** [Protocol and SDKs](docs/compatibility.md)
- **Validation:** [Tests and scope](docs/validation.md) · [Official conformance](docs/conformance.md)
- **Project:** [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Changelog](https://github.com/dugan-dev/MCP/releases)

## License

[MIT](LICENSE). Source attribution and third-party terms are retained in [NOTICE](NOTICE).
