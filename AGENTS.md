# MCP

先读 README、PROVENANCE.md、docs/validation.md。MCP 0.0.1 是独立 C++20 client/server 库；公开入口为 `<mcp/all.h>`、命名空间为 `mcp`、安装 target 为 `MCP::MCP`，不依赖宿主业务库、LLM 或 UI。

- CMake 3.25+、vcpkg、官方 UniMemory；同一程序只能使用一致的 allocator profile。
- 公开头文件置于 `include/mcp`，内存资源桥接为 `<mcp/memory.h>` 和 `mcp::memory`；wire/protocol、取消、生命周期、预算、认证和传输行为必须有测试支持。
- PRIVATE 静态库依赖也属于安装消费者闭包。不得只验证源码树构建而跳过安装后调用。
- 原始来源、MIT 与第三方声明随源码和发行包保留。
- 默认测试只使用合成数据、回环服务和子进程；真实服务测试明确区分连接、发现、调用与结果。
- 不读取或输出凭证，不扫描其他工作区；build 与用户配置不能进入源码发布包。
- 主 agent 负责设计/审查，实现 GPT-6.1-sol/high，独立测试 GPT-6-luna/high。测试失败先定位原因，不放宽协议断言换取通过。
