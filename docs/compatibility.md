# Protocol and SDK compatibility

MCP 0.0.1 implements protocol revision **2026-07-28**. The official specification, rather than the behavior of one SDK, defines the wire contract.

## Protocol scope

The current protocol carries client metadata/capabilities on requests and uses `server/discover`. This library does not implement the older `initialize` / `notifications/initialized` session lifecycle. A client or server that supports only the older lifecycle needs an adapter or its own compatible SDK; a matching JSON-RPC envelope alone is insufficient.

Tools, resources, prompts, completion, notifications, input workflows, authorization helpers and task/skill extensions have distinct capabilities. An application must advertise only the features it actually implements. Extensions remain optional and require peer support.

Primary references used for this release:

| Reference | Fixed revision |
| --- | --- |
| [Official specification](https://github.com/modelcontextprotocol/modelcontextprotocol/tree/1155ca460e5097019f32769b38dcba194432e2fb) | `1155ca460e5097019f32769b38dcba194432e2fb` |
| [Official Python SDK](https://github.com/modelcontextprotocol/python-sdk/tree/2af9e3089ac66b35c1550c500aae86a82efb1e17) | `2af9e3089ac66b35c1550c500aae86a82efb1e17` |
| [Official Go SDK](https://github.com/modelcontextprotocol/go-sdk/tree/3e9d812b181433e7ad88b629ba40a12fdaad94a9) | `3e9d812b181433e7ad88b629ba40a12fdaad94a9` |
| [Official Rust SDK](https://github.com/modelcontextprotocol/rust-sdk/tree/50fba5c2848d3ef9d0df0efacb8a7d001914f40e) | `50fba5c2848d3ef9d0df0efacb8a7d001914f40e` |
| [cpp-mcp](https://github.com/hkr04/cpp-mcp/tree/faa7cdcd85c671e61a107b03c6cf0a860394fc10) | `faa7cdcd85c671e61a107b03c6cf0a860394fc10` |

The retained official JSON Schema fixture is checked against the stable specification. Its source and license are recorded in [NOTICE](../NOTICE). SDKs are external test dependencies, not bundled runtime code.

## Comparison

| Aspect | MCP 0.0.1 | Official Python / Go / Rust | Inspected cpp-mcp |
| --- | --- | --- | --- |
| Primary interface | C++20, explicit client/server/transport | Language-native SDK APIs | C++ client/server API |
| Protocol lifecycle | 2026-07-28 only | Modern and legacy paths; explicit mode matters | 2025-03-26 initialize lifecycle |
| Local execution | Boost.Process V2, process-tree ownership | SDK/language process integration | Its own stdio/process implementation |
| HTTP implementation | libcurl client, Asio/Beast server | Language-specific HTTP stacks | Different HTTP dependency stack |
| Allocation contract | UniMemory-backed public JSON | Language/runtime allocation | Different allocation interface |

Protocol versions make a blanket drop-in replacement claim inappropriate. cpp-mcp offers a different dependency footprint and may fit legacy clients better. Neither line counts nor the number of dependencies establish correctness or performance. Use the [reproducible tests](validation.md) and your workload to evaluate a choice.

For official SDK tests, Python explicitly selects `mode="2026-07-28"`, Rust selects its discovery lifecycle and Go asserts the negotiated version. Legacy fallback cannot make these tests pass accidentally. Core interop does not certify every authorization, task, notification or extension combination.

## Public version policy

The public source interface is `<mcp/...>`, namespace `mcp`, and target `MCP::MCP`. The initial public version is **0.0.1**. Earlier private extraction names and Git history are not its public compatibility baseline.

Patch releases intend to preserve documented valid source usage. Bug fixes may reject malformed input. Breaking public changes require a version change and migration guidance. There is no binary ABI guarantee in 0.0.1; rebuild consumers when toolchains, dependency ABIs or layouts change. See the [API contract](api-reference.md).
