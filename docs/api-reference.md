# Public API contract

Include `<mcp/all.h>` for the complete interface or a specific header from `include/mcp`. Public symbols use `mcp`; optional task/skill helpers use `mcp::extensions`. Link `MCP::MCP`.

## Values and memory

`mcp::Json` is an allocator-aware nlohmann JSON specialization, not `nlohmann::json`. Use `parse_json`, `serialize_json` and `text` at boundaries. `text` returns a borrowed string view; copy it before destroying or changing its JSON owner.

`JsonStorage` is shared ownership of a `std::pmr::memory_resource`. A custom resource must be thread-safe and must own or otherwise preserve every backing buffer until the final JSON allocation is released. A shared pointer to a resource does not extend the lifetime of a separate stack buffer used by that resource.

`JsonScope` selects the allocation resource on the current thread for its lexical lifetime. Allocations retain their resource and may outlive the scope or client, including destruction on another thread. Do not share a `JsonScope` across threads or destroy nested scopes out of order. `memory::default_resource()` owns an official UniMemory allocation context.

All libraries in a process must use a compatible UniMemory profile. The installed package checks its profile and the exact nlohmann/json version used for the build. Compiler, runtime and dependency ABI compatibility remain the application's responsibility.

## Lifetime and threading

| Object or argument | Contract |
| --- | --- |
| `Client(std::shared_ptr<Transport>)` | Retains the transport. Explicit `close()` closes the underlying transport, including other users sharing it. Destruction releases ownership; it does not force-close a transport still owned elsewhere. |
| `HttpServer(Server&)` | Borrows the server. Stop and destroy the HTTP server before destroying that server. |
| `TaskClient`, `SkillsClient` | Borrow their `Client&`; it must outlive them and all ongoing calls. |
| Handler `RequestContext&` | Valid only during the current dispatch. Do not retain it or use it from detached work. |
| Callback `const Json&`, `SseDecoder` callback string views and context accessors | Borrowed for the call. Copy values that must survive the callback. |
| Registered `std::function` | Owns its captures according to normal C++ rules; reference captures do not extend lifetimes. |

Built-in transports support concurrent exchanges with request correlation. Server dispatch and transport callbacks may run concurrently; applications must synchronize shared state in handlers, token providers, authorizers and notification callbacks. Serialize mutation/use of OAuth helper objects unless the specific interface documents otherwise. Do not destroy an object concurrently with a method using it.

Callbacks should complete promptly. Avoid calling `HttpServer::stop()` from its own worker callback: shutdown joins worker threads. Arbitrary callback reentrancy and a callback blocking on its own request are not supported contracts.

## Requests, cancellation and errors

`RequestOptions` supplies a positive `timeout`, a cooperative `stop_token` and protocol metadata. Client pagination/input continuation and `TaskClient` creation/polling carry one budget through their steps. OAuth discovery and exchange helpers apply the supplied budget to each `AuthExchange` separately; callers needing an overall login deadline must enforce it in their exchange callback. Transport work observes cancellation; cancellation does not undo external side effects. A caller-provided callback that blocks cannot be forcibly interrupted. Check the stop token inside long-running handlers.

| Result | Meaning |
| --- | --- |
| `RpcError` | Peer/protocol failure; `code()` preserves the integer code and `data()` preserves structured details. |
| `TransportError` | Connection, timeout, cancellation, size limit, invalid response or closed transport. Inspect `kind()`. |
| Tool result with `isError: true` | Application/tool failure carried as a valid protocol result. |
| `std::invalid_argument`, JSON exceptions | Invalid local inputs or value conversions, depending on the API. |
| `std::bad_alloc` | Allocation failure; not converted into an unlimited retry. |

The `ErrorCode` enumeration names common codes; it is not an exhaustive list of all extension errors. Exceptions from application callbacks must be handled according to their invocation context; do not assume every local exception is a peer error.

## Discovery, lists and descriptors

`discover()` explicitly requests server capabilities. `ClientOptions` supplies the client information and capabilities attached to requests. There is no implicit legacy initialize handshake.

`list_tools`, `list_resources`, `list_resource_templates` and `list_prompts` return aggregated **arrays**. They enforce page, item and byte limits; exceeding a limit fails rather than silently truncating. Use `request()` for a raw page/result envelope and explicit cursors. Keep authorization contexts separate when sharing or caching discovery results.

Descriptor `metadata` fields are merged at the descriptor's **top level**, not automatically nested under `_meta`. Use them for unmodeled extension fields such as `title`, `icons` or `_meta`. Set modeled fields through their typed members. In particular, use `Tool::output_schema`, not `metadata["outputSchema"]`, to enable server output validation. Metadata is an extension escape hatch, not a second typed registration API.

Malformed `x-mcp-header` annotations cause the affected tool to be excluded from discovered tools. A fresh tools list replaces transport header mappings and cached output schemas. Manually configuring `HttpTransport::set_tool_schema(s)` remains strict and rejects invalid annotations.

## Extensibility

- `Client::request()` carries raw methods and protocol results.
- `Server::add_extension()` registers extension capabilities and handlers.
- Deriving from `Transport` supports application-specific communication. Implement correlation, remaining timeout and cancellation, bounded input/output, and `close() noexcept` that wakes pending operations. Deliver notifications for an exchange before it returns; do not retain borrowed callbacks or arguments past their valid lifetime.
- `features` and `extensions` provide task, input and skill helpers. Task records do not themselves schedule work or persist it across restarts. Applications provide execution and durable storage when needed.

Validate and authorize raw input-handler payloads before acting on them. Helpers do not turn untrusted tool input or model text into authorization.

## Authentication

The application supplies trusted identity attributes to dispatch and subscriptions. Never copy identity/owner values from unverified request JSON into trusted attributes. Task access uses the authenticated owner's key.

Authorization helpers support discovery, issuer/audience, expiry and scope checks. The configured verifier still has to verify signatures or introspect tokens appropriately. MCP is not an identity provider, secret vault or general JWT signature engine. Keep TLS verification enabled and choose exposure/origin/host policy deliberately.

## Automatic HTTP authorization

`OAuthTransport` composes the HTTP transport and `OAuthClient`. Set `OAuthTransportOptions::http.url`, the redirect URI, and an authorization callback returning the actual callback parameters. The transport bounds authorization retries (default two), preserves the request deadline across metadata/token requests, validates issuer/state/resource binding, selects advertised token authentication and handles scope escalation. Token refresh uses the same resource-bound OAuth client. Concurrent unauthenticated requests share an authorization gate; ordinary authenticated exchanges and notification callbacks do not hold that gate.

The callback is application code and must implement its own cancellation while waiting for the user. Closing the transport closes its active HTTP transport; a user callback that ignores its `RequestOptions` cannot be forcibly interrupted. Token persistence, redirect handling, client registration policy and token verification remain application responsibilities. Pre-registered credentials require `credential_issuer`; the automatic transport rejects unbound credentials before any network request. Authorization callbacks must not recursively issue MCP requests on the same transport: that is rejected with `AuthError("reentrant_authorization", ...)`, while ordinary notification callback reentry remains supported.

Plain `HttpTransport` retains its existing HTTP error behavior. Opting into `HttpClientOptions::capture_authorization_challenges` exposes 401/403 status and headers through `HttpAuthorizationError`; `OAuthTransport` enables this internally. It does not disable TLS verification or follow authorization redirects automatically. `AuthResponse::headers` contains lowercase response header names, including redirect locations.

The complete examples are `examples/oauth_client.cpp` and the native automatic-authorization regressions. Rebuild consumers after changes to public aggregate layouts; this source update does not imply an ABI-compatible replacement for a prebuilt 0.0.1 binary.

## Compatibility policy

The documented headers and target above form the source interface. Patch and minor releases in the 1.x series are intended to preserve documented names, signatures and valid usage. Fixes can reject invalid input previously accepted. Breaking public changes require an explicit version change and migration guidance.

Binary ABI stability across toolchains and dependency changes is **not** promised: public STL types, JSON templates, aggregate layouts and virtual interfaces depend on the toolchain and dependencies. Rebuild consumers when these change. `src/`, `mcp::detail` and internal CMake helper functions are not public interfaces.
