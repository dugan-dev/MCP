# MCP

[![Build](https://github.com/dugan-dev/MCP/actions/workflows/build.yml/badge.svg)](https://github.com/dugan-dev/MCP/actions/workflows/build.yml) [![Version](https://img.shields.io/badge/version-0.0.1-blue)](CHANGELOG.md) [![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](CMakeLists.txt) [![CMake](https://img.shields.io/badge/CMake-3.25%2B-blue)](CMakePresets.json) [![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

**用于 Model Context Protocol 的 C++20 客户端与服务端库。**

提供工具、资源与提示词，通过本地进程或 HTTP 连接应用，让协议、传输与业务逻辑各司其职。

[English](README.md) · **简体中文**

<details>
<summary>目录</summary>

- [特性](#特性)
- [快速开始](#快速开始)
- [平台](#平台)
- [能力](#能力)
- [性能](#性能)
- [构建与安装](#构建与安装)
- [集成](#集成)
- [文档](#文档)
- [许可证](#许可证)

</details>

## 特性

- **客户端与服务端**：工具、资源、提示词、能力发现与补全。
- **本地与远程连接**：子进程 stdio、HTTP/TLS 与 SSE 通知。
- **可控执行**：取消、超时预算、有界队列与进程树清理。
- **按需扩展**：自定义传输、处理器、身份验证、任务与技能。
- **统一内存管理**：以 UniMemory 支持 JSON 分配，明确对象所有权。
- **直接集成**：`<mcp/all.h>`、`mcp` 命名空间、`MCP::MCP` CMake 目标。

**0.0.1** 实现 **2026-07-28** 版协议，不支持旧版 `initialize`/`initialized` 会话。接入前请确认客户端协议版本，详见[兼容性](docs/compatibility.md)。

## 快速开始

```cpp
#include <mcp/all.h>
#include <iostream>

int main() {
    mcp::Server server;
    auto result = server.dispatch(mcp::make_request(1, "server/discover"));
    std::cout << mcp::serialize_json(result) << '\n';
}
```

安装后直接用 CLI 查看服务端工具：

```sh
mcp --exec /absolute/path/to/server tools list
mcp --url http://127.0.0.1:8080/mcp tools list
```

[创建工具并连接客户端 →](docs/getting-started.md)

## 平台

| 平台 | 构建环境 | 验证范围 |
| --- | --- | --- |
| Windows | MSVC，C++20 | 本机 Debug/Release 测试与安装消费验证 |
| Linux | GCC 或 Clang，C++20 | 本机子进程检查；已配置完整 CI |
| macOS | Apple Clang，C++20 | 已配置完整 CI，执行结果见 Build 徽章 |

Build 徽章显示当前 CI 状态。[验证说明](docs/validation.md)区分已执行的检查与支持的配置。

## 能力

| 层级 | 公共头文件 | 职责 |
| --- | --- | --- |
| 内存与数据 | `memory.h`、`json.h`、`protocol.h` | 分配、JSON、消息、限制与错误 |
| 描述与校验 | `tool.h`、`resource.h`、`prompt.h`、`schema.h` | 声明并校验应用接口 |
| 传输 | `transport.h`、`stdio.h`、`http.h`、`sse.h` | 进程管道、HTTP/TLS 与事件流 |
| 应用端点 | `client.h`、`server.h` | 发现、调用、分发与通知 |
| 可选能力 | `auth.h`、`features.h`、`extensions.h` | 授权辅助、输入流程、任务与技能 |

以上是同一个库内的逻辑层。工具业务、持久存储和身份策略由应用提供。[查看架构与依赖 →](docs/architecture.md)

## 性能

MCP 复用 HTTP 连接，限制传输缓存，支持并发请求关联和可配置的内存资源。同步接口通过传输层 I/O 执行，并使用明确的请求预算。

实际性能取决于服务端、传输、数据量与分配器配置。仓库提供压力测试，不宣称未经测量的速度或内存优势。

## 构建与安装

准备 C++20 编译器、CMake 3.25+，将 `VCPKG_ROOT` 指向 vcpkg 目录：

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/MCP-release --config Release
```

只有 **`debug`** 和 **`release`** 两个预设。构建目录为 `build/MCP-<预设>`，安装目录为 `build/install/<预设>`；两者共享 `build/dependencies` 中的依赖。

| 目标 | 用途 |
| --- | --- |
| `lib_mcp` | MCP 库，使用者链接 `MCP::MCP` |
| **`exe_cli`** | **启动 `mcp` 命令行程序** |
| `example_stdio_server`、`example_stdio_client` | 本地进程示例 |
| `example_http_server`、`example_http_client` | HTTP 示例 |
| `test_*`、`fixture_*` | 测试与测试辅助程序 |

MCP 没有 GUI 程序。固定版本的 vcpkg 清单负责依赖；Boost.Process 使用 **V2** 和 **`std::filesystem`**，不依赖 Boost.Filesystem。[构建选项与排错 →](docs/guides/build.md)

## 集成

```cmake
find_package(MCP 0.0.1 CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE MCP::MCP)
```

将安装目录加入 `CMAKE_PREFIX_PATH`，使用一致的依赖工具链、架构与运行库。源码集成使用 `add_subdirectory`，链接目标不变。

请使用 `mcp::Json`，保持 UniMemory 配置一致，并在添加回调或自定义传输前阅读[所有权与并发契约](docs/api-reference.md)。

## 文档

- [入门](docs/getting-started.md)：服务端、客户端与 CLI 示例。
- [架构](docs/architecture.md)：层级、依赖与仓库结构。
- [接口契约](docs/api-reference.md)：生命周期、线程、取消与错误。
- [兼容性](docs/compatibility.md)：协议、官方 SDK 与版本策略。
- [构建指南](docs/guides/build.md)：依赖、目标与安装。
- [验证说明](docs/validation.md)：可复现命令与边界。
- [参与贡献](CONTRIBUTING.md) · [安全](SECURITY.md) · [更新记录](CHANGELOG.md)。

## 许可证

[MIT](LICENSE)。来源与第三方声明保留在 [NOTICE](NOTICE)、[licenses](https://github.com/dugan-dev/MCP/tree/main/licenses) 和 [PROVENANCE.md](PROVENANCE.md) 中。公开仓库从 0.0.1 建立新历史，代码来源仍完整记录。
