# MCP

[![Build](https://github.com/dugan-dev/MCP/actions/workflows/build.yml/badge.svg)](https://github.com/dugan-dev/MCP/actions/workflows/build.yml) [![Version](https://img.shields.io/badge/version-0.0.1-blue)](CHANGELOG.md) [![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](CMakeLists.txt) [![CMake](https://img.shields.io/badge/CMake-3.25%2B-blue)](CMakePresets.json) [![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

**A C++20 client and server library for the Model Context Protocol.**

Expose tools, resources and prompts. Connect over local processes or HTTP. Keep protocol, transport and application logic separate.

**English** · [简体中文](README.zh-CN.md)

<details>
<summary>Contents</summary>

- [Features](#features)
- [Quick Start](#quick-start)
- [Platforms](#platforms)
- [Capabilities](#capabilities)
- [Performance](#performance)
- [Build and Install](#build-and-install)
- [Integration](#integration)
- [Documentation](#documentation)
- [License](#license)

</details>

## Features

- **Client and server** — tools, resources, prompts, discovery and completions.
- **Local and remote** — subprocess stdio, HTTP/TLS and SSE notifications.
- **Controlled execution** — cancellation, time budgets, bounded queues and process tree cleanup.
- **Extensible** — custom transports, handlers, authentication hooks, tasks and skills.
- **Consistent allocation** — UniMemory-backed JSON with explicit ownership.
- **Simple integration** — `<mcp/all.h>`, namespace `mcp`, CMake target `MCP::MCP`.

Version **0.0.1** implements the **2026-07-28** protocol. Legacy `initialize`/`initialized` sessions are not supported. Check your client's protocol version before integrating; see [compatibility](docs/compatibility.md).

## Quick Start

```cpp
#include <mcp/all.h>
#include <iostream>

int main() {
    mcp::Server server;
    auto result = server.dispatch(mcp::make_request(1, "server/discover"));
    std::cout << mcp::serialize_json(result) << '\n';
}
```

Use the installed CLI to inspect a server:

```sh
mcp --exec /absolute/path/to/server tools list
mcp --url http://127.0.0.1:8080/mcp tools list
```

[Create a tool and connect a client →](docs/getting-started.md)

## Platforms

| Platform | Build | Validation |
| --- | --- | --- |
| Windows | MSVC, C++20 | Native Debug/Release tests and installed consumers |
| Linux | GCC or Clang, C++20 | Native subprocess checks; full CI configured |
| macOS | Apple Clang, C++20 | Full CI configured; results in the build badge |

The build badge reports the current CI result. [Validation](docs/validation.md) distinguishes executed checks from supported configurations.

## Capabilities

| Layer | Public headers | Purpose |
| --- | --- | --- |
| Memory and values | `memory.h`, `json.h`, `protocol.h` | Allocation, JSON, messages, limits and errors |
| Descriptors and validation | `tool.h`, `resource.h`, `prompt.h`, `schema.h` | Declare and validate interfaces |
| Transports | `transport.h`, `stdio.h`, `http.h`, `sse.h` | Process pipes, HTTP/TLS and event streams |
| Application endpoints | `client.h`, `server.h` | Discover, invoke, dispatch and notify |
| Optional features | `auth.h`, `features.h`, `extensions.h` | Authorization helpers, input workflows, tasks and skills |

These are logical layers in one library. Applications supply their tools, storage and identity policy. [Architecture and dependencies →](docs/architecture.md)

## Performance

MCP reuses HTTP connections, bounds transport buffers, correlates concurrent requests and supports configurable allocation resources. Blocking public calls use transport I/O with explicit request budgets.

Performance depends on the server, transport, payload and allocator profile. Stress tests are included; no measured speed or memory advantage over other SDKs is claimed.

## Build and Install

Install a C++20 compiler and CMake 3.25+. Set `VCPKG_ROOT` to your vcpkg checkout:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/MCP-release --config Release
```

There are only two presets: **`debug`** and **`release`**. Outputs stay in `build/MCP-<preset>`; installation goes to `build/install/<preset>`. Both share vcpkg packages in `build/dependencies`.

| Target | What it builds |
| --- | --- |
| `lib_mcp` | MCP library; consumers link `MCP::MCP` |
| **`exe_cli`** | **`mcp` command-line program** |
| `example_stdio_server`, `example_stdio_client` | Local process examples |
| `example_http_server`, `example_http_client` | HTTP examples |
| `test_*`, `fixture_*` | Tests and test helpers |

MCP has no GUI executable. Dependencies use a pinned vcpkg manifest. Boost.Process **V2** uses **`std::filesystem`**, without Boost.Filesystem. [Build options and troubleshooting →](docs/guides/build.md)

## Integration

```cmake
find_package(MCP 0.0.1 CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE MCP::MCP)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix and use the same dependency toolchain, architecture and runtime. Source embedding uses `add_subdirectory` and the same target.

Use `mcp::Json`, share a compatible UniMemory profile, and read the [ownership and concurrency contract](docs/api-reference.md) before adding callbacks or custom transports.

## Documentation

- [Getting started](docs/getting-started.md) — server, client and CLI examples.
- [Architecture](docs/architecture.md) — layers, dependencies and repository layout.
- [API contract](docs/api-reference.md) — lifetime, threading, cancellation and errors.
- [Compatibility](docs/compatibility.md) — specification, official SDKs and version policy.
- [Build guide](docs/guides/build.md) — dependencies, targets and installation.
- [Validation](docs/validation.md) — reproducible checks and limits.
- [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Changelog](CHANGELOG.md).

## License

[MIT](LICENSE). Attribution and dependency terms remain in [NOTICE](NOTICE), [licenses](https://github.com/dugan-dev/MCP/tree/main/licenses) and [PROVENANCE.md](PROVENANCE.md). Public history begins at 0.0.1; source provenance remains documented.
