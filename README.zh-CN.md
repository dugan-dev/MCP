# MCP

[![Build](https://github.com/dugan-dev/MCP/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dugan-dev/MCP/actions/workflows/build.yml) [![Version](https://img.shields.io/badge/version-0.0.1-blue)](CHANGELOG.md) [![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://isocpp.org/std/the-standard) [![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

用于构建 MCP 客户端与服务端的 C++20 库，通过 stdio 或 HTTP 提供工具、资源与提示词。

[English](README.md) · **简体中文**

实现 **2026-07-28** 版协议，不支持旧版 `initialize` 会话。[兼容性说明 →](docs/compatibility.md)

## 构建与安装

需要 **C++20**、**CMake 3.25+** 和 **vcpkg**。将 `VCPKG_ROOT` 指向 vcpkg 目录，在源码目录执行：

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/MCP-release --config Release
```

默认安装到 `build/install/release`。调试构建改用 `debug` / `Debug`。**`exe_cli`** 目标生成 **`mcp`** 程序，将安装目录下的 `bin` 加入 `PATH` 即可使用。

## 快速开始

保存为 `main.cpp`，创建一个名为 `hello` 的工具：

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

标准输出用于协议消息，应用日志请写入标准错误。

在项目的 `CMakeLists.txt` 中链接：

```cmake
cmake_minimum_required(VERSION 3.25)
project(HelloMCP LANGUAGES CXX)

find_package(MCP 0.0.1 CONFIG REQUIRED)
add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE MCP::MCP)
```

将 MCP 安装目录加入 `CMAKE_PREFIX_PATH`，使用匹配的 vcpkg 依赖配置。项目配置与运行库部署见[构建指南](docs/guides/build.md)。

## 调用工具

CLI 会启动你的程序并调用工具：

```sh
mcp --exec /absolute/path/to/hello tools list
mcp --exec /absolute/path/to/hello tools call hello
```

调用会返回包含 `Hello, MCP!` 的文本结果。查看 HTTP 服务端的工具：

```sh
mcp --url http://127.0.0.1:8080/mcp tools list
```

更多命令见 `mcp --help`，也可以使用 [C++ 客户端](docs/getting-started.md#connect-a-client)。

## 文档

- **使用：** [入门](docs/getting-started.md) · [构建指南](docs/guides/build.md)
- **参考：** [接口](docs/api-reference.md) · [架构](docs/architecture.md) · [兼容性](docs/compatibility.md)
- **项目：** [验证记录](docs/validation.md) · [参与贡献](CONTRIBUTING.md) · [安全](SECURITY.md)

## 许可证

[MIT](LICENSE)。代码来源与第三方声明保留在 [NOTICE](NOTICE) 和 [PROVENANCE.md](PROVENANCE.md) 中。
