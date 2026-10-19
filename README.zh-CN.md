# MCP

[![Build](https://github.com/dugan-dev/MCP/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dugan-dev/MCP/actions/workflows/build.yml?query=branch%3Amain) [![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)](https://github.com/dugan-dev/MCP/releases) [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE) [![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/std/the-standard) [![CMake](https://img.shields.io/badge/CMake-3.25%2B-green.svg)](https://cmake.org/) [![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#平台)

现代 C++20 MCP 客户端与服务端库，通过 stdio 或 HTTP 提供工具、资源与提示词。

[English](README.md) · **简体中文**

<details>
<summary>目录</summary>

- [特点](#特点)
- [快速开始](#快速开始)
- [平台](#平台) · [功能](#功能)
- [性能](#性能)
- [构建与安装](#构建与安装) · [项目集成](#项目集成)
- [文档](#文档) · [许可证](#许可证)

</details>

## 特点

- **🚀 现代 C++20**：类型化的客户端、服务端与传输接口。
- **🔧 工具**：注册处理函数，校验 JSON 输入与输出结构。
- **📚 资源与提示词**：提供内容访问和可复用的提示词模板。
- **🔄 统一传输**：本地子进程、HTTP/TLS 与 SSE 通知。
- **⏱️ 请求控制**：超时、取消与可配置的请求限制。
- **🧩 按需扩展**：授权辅助、输入流程、任务与技能。
- **🧠 统一内存**：使用 UniMemory 管理 JSON 内存分配。
- **📦 简单集成**：包含 `<mcp/all.h>`，链接 `MCP::MCP`。

**1.0.0** 实现 **2026-07-28** 版协议，不支持旧版 `initialize` 会话。[兼容性说明 →](docs/compatibility.md)

## 快速开始

### 基本用法

保存为 `main.cpp`，创建一个名为 `hello` 的工具服务：

```cpp
#include <mcp/all.h>
#include <iostream>

int main() {
  mcp::Server server;
  server.add_tool({"hello", "Say hello"}, [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_result("Hello, MCP!"); });
  mcp::run_stdio(server, std::cin, std::cout);
}
```

标准输出用于协议消息，日志请写入标准错误。下面的工具、资源和提示词注册代码放在 `run_stdio` 之前。

### 工具

声明输入结构，返回文本结果：

```cpp
server.add_tool({"echo", "Return a message", {{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}, {"required", {"message"}}}},
                [](const mcp::Json& args, mcp::RequestContext&) { return mcp::text_result(mcp::text(args["message"])); });
```

### 资源

通过 URI 提供内容：

```cpp
server.add_resource({"example://hello", "hello", "Greeting", "text/plain"},
                    [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_resource("example://hello", "Hello, MCP!"); });
```

### 提示词

创建带有必填参数的提示词模板：

```cpp
server.add_prompt({"greet", "Generate a greeting", mcp::Json::array({{{"name", "name"}, {"required", true}}})},
                  [](const mcp::Json& args, mcp::RequestContext&) { return mcp::prompt_result("Hello, " + std::string(mcp::text(args["name"]))); });
```

### 客户端

在另一个程序中包含 `<mcp/all.h>`，连接编译好的服务端：

```cpp
mcp::ProcessOptions process;
process.executable = "/absolute/path/to/hello";

auto transport = std::make_shared<mcp::StdioTransport>(process);
mcp::Client client(transport);

auto info = client.discover();
auto tools = client.list_tools();
auto greeting = client.call_tool("hello", mcp::Json::object());
```

如果添加了上面的注册代码，还可以调用工具、读取资源、获取提示词：

```cpp
auto echoed = client.call_tool("echo", {{"message", "Hello"}});
auto resource = client.read_resource("example://hello");
auto prompt = client.get_prompt("greet", {{"name", "Ada"}});
```

使用完毕后调用 `client.close()`，关闭它启动的子进程。

### HTTP 服务端

将基本用法中的 `run_stdio` 替换为以下代码。监听结束前保持 `server` 存活：

```cpp
mcp::HttpServerOptions options;
options.port = 8080;

mcp::HttpServer listener(server, options);
listener.start();
std::cin.get();  // Press Enter to stop.
listener.stop();
```

### HTTP 客户端

将子进程传输换为 HTTP，客户端接口保持一致：

```cpp
auto transport = std::make_shared<mcp::HttpTransport>(mcp::HttpClientOptions{"http://127.0.0.1:8080/mcp"});
mcp::Client client(transport);

auto info = client.discover();
auto greeting = client.call_tool("hello", mcp::Json::object());
```

### 请求选项

为已连接的客户端设置超时和取消令牌：

```cpp
std::stop_source stop;
mcp::RequestOptions request;
request.timeout = std::chrono::seconds(5);
request.stop = stop.get_token();

auto result = client.call_tool("hello", mcp::Json::object(), request);
```

其他线程可以调用 `stop.request_stop()`。耗时工具的处理函数应检查 `context.throw_if_cancelled()`。

### 命令行

安装后，将 `build/install/release/bin` 加入 `PATH`：

```sh
mcp --exec /absolute/path/to/hello tools list
mcp --exec /absolute/path/to/hello tools call hello
mcp --url http://127.0.0.1:8080/mcp tools list
mcp --help
```

`tools call hello` 返回 `Hello, MCP!`。Windows 下使用 `hello.exe` 的路径。

## 平台

| 平台 | 编译器 | 验证范围 |
| --- | --- | --- |
| Windows x64 | MSVC | 本机 Debug 与 Release |
| Linux x64 | GCC / Clang | 本机 GCC Release |
| macOS | Apple Clang (Xcode 26+) | 已配置 CI，结果见 Build 徽章 |
| Android / iOS | — | 尚未进行设备验证 |

CI 覆盖静态库、动态库和 Debug/Release。[已执行的验证 →](docs/validation.md)

## 功能

| 功能 | Stdio | HTTP |
| --- | :---: | :---: |
| 工具、资源、提示词 | ✔ | ✔ |
| 并发请求与通知 | ✔ | ✔ |
| 超时与取消 | ✔ | ✔ |
| 管理并清理子进程 | ✔ | — |
| TLS 与 HTTP 授权 | — | ✔ |

可选扩展需要通信双方共同支持；工具逻辑和身份策略由应用提供。

## 性能

复用 HTTP 连接，支持并发请求与有界缓存，使用 UniMemory 管理内存。参见[性能测量](docs/performance.md)与[测试验证](docs/validation.md)，暂未发布跨 SDK 对比数据。

## 构建与安装

需要 **C++20**、**CMake 3.25+** 和 **vcpkg**。将 `VCPKG_ROOT` 指向 vcpkg 目录，在源码目录执行：

```sh
git clone --recurse-submodules https://github.com/dugan-dev/MCP.git
cd MCP
cmake --preset release
cmake --build --preset release
cmake --install build/MCP-release --config Release
```

默认安装到 `build/install/release`。调试构建使用 `debug` / `Debug`。**`exe_cli`** 目标生成 **`mcp`** 命令行程序。

## 项目集成

在项目的 `CMakeLists.txt` 中链接 MCP：

```cmake
cmake_minimum_required(VERSION 3.25)
project(HelloMCP LANGUAGES CXX)

find_package(MCP 1.0.0 CONFIG REQUIRED)
add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE MCP::MCP)
```

将 MCP 安装目录加入 `CMAKE_PREFIX_PATH`，使用匹配的依赖工具链。项目配置与运行库部署见[构建指南](docs/build.md)。

## 文档

- **开始使用：** [构建和集成](docs/getting-started.md) · [示例](https://github.com/dugan-dev/MCP/tree/main/examples)
- **配置：** [构建指南](docs/build.md) · [依赖](docs/dependencies.md)
- **接口：** [接口契约](docs/api-reference.md) · [功能示例](docs/features.md)
- **设计：** [架构与模块](docs/architecture.md)
- **兼容性：** [协议与 SDK](docs/compatibility.md)
- **验证：** [测试与范围](docs/validation.md) · [官方一致性测试](docs/conformance.md)
- **项目：** [参与贡献](CONTRIBUTING.md) · [安全](SECURITY.md) · [更新记录](https://github.com/dugan-dev/MCP/releases)

## 许可证

[MIT](LICENSE)。代码来源与第三方声明保留在 [NOTICE](NOTICE) 中。
