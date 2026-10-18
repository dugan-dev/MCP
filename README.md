# MCP

[![Build](https://github.com/dugan-dev/MCP/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dugan-dev/MCP/actions/workflows/build.yml) [![Version](https://img.shields.io/badge/version-0.0.1-blue)](CHANGELOG.md) [![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://isocpp.org/std/the-standard) [![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

A C++20 library for building MCP clients and servers. Expose tools, resources and prompts over stdio or HTTP.

**English** · [简体中文](README.zh-CN.md)

Implements protocol **2026-07-28**; legacy `initialize` sessions are not supported. [Compatibility →](docs/compatibility.md)

## Build and Install

Requires **C++20**, **CMake 3.25+** and **vcpkg**. Set `VCPKG_ROOT` to your vcpkg checkout, then run from the source directory:

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/MCP-release --config Release
```

Installed to `build/install/release`. For Debug, use `debug` / `Debug`. The **`exe_cli`** target builds **`mcp`**; add the installed `bin` directory to `PATH` to use it.

## Quick Start

Save as `main.cpp` to expose a tool named `hello`:

```cpp
#include <mcp/all.h>
#include <iostream>

int main() {
    mcp::Server server;
    server.add_tool({"hello", "Say hello"},
        [](const mcp::Json&, mcp::RequestContext&) {
            return mcp::text_result("Hello, MCP!");
        });
    mcp::run_stdio(server, std::cin, std::cout);
}
```

Stdout carries protocol messages; write application logs to stderr.

Link the library in your project's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(HelloMCP LANGUAGES CXX)

find_package(MCP 0.0.1 CONFIG REQUIRED)
add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE MCP::MCP)
```

Set `CMAKE_PREFIX_PATH` to the MCP installation and use the matching vcpkg dependency configuration. See the [build guide](docs/guides/build.md) for consumer setup and runtime deployment.

## Call a Tool

The CLI launches your executable and calls its tool:

```sh
mcp --exec /absolute/path/to/hello tools list
mcp --exec /absolute/path/to/hello tools call hello
```

The call returns a text result containing `Hello, MCP!`. To inspect an HTTP server:

```sh
mcp --url http://127.0.0.1:8080/mcp tools list
```

Use `mcp --help` for more commands, or follow the [C++ client example](docs/getting-started.md#connect-a-client).

## Documentation

- **Usage:** [Getting started](docs/getting-started.md) · [Build guide](docs/guides/build.md)
- **Reference:** [API](docs/api-reference.md) · [Architecture](docs/architecture.md) · [Compatibility](docs/compatibility.md)
- **Project:** [Validation](docs/validation.md) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md)

## License

[MIT](LICENSE). Source attribution and third-party terms are retained in [NOTICE](NOTICE) and [PROVENANCE.md](PROVENANCE.md).
