# Feature examples

Use `<mcp/all.h>` and link `MCP::MCP`. [features.cpp](../examples/features.cpp) is a complete, offline program: registrations, local client/server calls, automatic pagination, notifications, input workflows and cancellation run together. It is compiled and executed by the native tests. [oauth_client.cpp](../examples/oauth_client.cpp) is a complete interactive authorization example; it requires an actual authorization service and redirect receiver.

## Tools, schemas and content

```cpp
server.add_tool({"echo", "Echo", {{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}, {"required", {"message"}}}},
                [](const mcp::Json& args, mcp::RequestContext&) { return mcp::text_result(mcp::text(args["message"])); });
auto result = client.call_tool("echo", {{"message", "Hello"}});
```

`Tool::output_schema` validates structured output. Return `structuredContent` alongside `content` for typed data; `Tool::metadata` preserves fields such as titles and icons. Schemas retain the JSON Schema 2020-12 vocabulary; external network references are not fetched automatically. `Schema::validate` throws for invalid values; `accepts` returns a boolean.

```cpp
mcp::Tool tool{"answer", "A structured result"};
tool.output_schema = mcp::Json{{"type", "object"}, {"properties", {{"value", {{"type", "integer"}}}}}, {"required", {"value"}}};
server.add_tool(tool, [](const mcp::Json&, mcp::RequestContext&) {
  auto result = mcp::text_result("42");
  result["structuredContent"] = {{"value", 42}};
  return result;
});
```

Content blocks support `text`, `image`, `audio`, `resource` and resource links according to the protocol schema. Media uses base64 `data` and a `mimeType`; embedded resources carry `uri` and `text` or `blob`. The offline example includes mixed content. An application failure can return `text_result("reason", true)`; malformed protocol parameters raise `RpcError` rather than pretending the tool succeeded.

## Resources, templates, prompts and completions

The offline example registers text/binary resources and a URI template with a resolver. Use `list_resources`, `list_resource_templates` and `read_resource`. Resolvers must reject unknown URIs; avoid treating arbitrary URIs as file paths.

```cpp
server.add_resource({"example://hello", "hello", "Greeting", "text/plain"},
                    [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_resource("example://hello", "Hello"); });
auto resource = client.read_resource("example://hello");
server.add_prompt({"greet", "Greeting"}, [](const mcp::Json&, mcp::RequestContext&) { return mcp::prompt_result("Hello"); });
auto prompt = client.get_prompt("greet");
```

Prompt arguments are named descriptors with optional `required`. Prompt `messages` can contain text, images or embedded resources, using the same content types as tools. The official fixture demonstrates each content type. `set_completion_handler` returns `completion.values`, `total` and `hasMore`; clients use `complete` with a `ref/prompt` or `ref/resource` reference and an argument name/value.

## Discovery, pagination, caching and notifications

`client.discover()` checks supported versions and capabilities. Every request carries its protocol version and client capabilities; the transport creates the matching HTTP headers. This library targets the modern protocol, so there is no `initialize` or separate `ping` operation. Discovery is optional on the wire but useful for applications that need capability information.

`ServerOptions::limits.page_size` controls server pages. Client list methods follow opaque cursors and enforce page, item and byte budgets. `cache_ttl_ms` and `cache_scope` produce protocol caching hints; applications decide whether to cache.

```cpp
auto subscription = server.subscribe(
    mcp::make_request(1, "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}, {"resourceSubscriptions", {"example://hello"}}}}}),
    [](const mcp::Json& notification) { /* consume the notification */ });
server.resource_updated("example://hello");
subscription->close();
```

Registration/removal produces matching list-change notices. `Client::listen` consumes the HTTP or stdio subscription stream with filters and `RequestOptions`; cancellation closes that listen operation. The acknowledgement precedes updates, and subscription IDs and filters are enforced. Unsubscribe by cancelling the listen or closing its subscription, not by sending removed legacy `resources/unsubscribe` messages.

## Progress, cancellation and logging

```cpp
mcp::RequestOptions options;
options.timeout = std::chrono::seconds(5);
options.meta = {{"progressToken", "job-1"}, {"io.modelcontextprotocol/logLevel", "info"}};
std::stop_source stop;
options.stop = stop.get_token();
auto result = client.call_tool("echo", {{"message", "Hello"}}, options, [](const mcp::Json& notification) { /* consume progress or logs */ });
```

Another thread can call `stop.request_stop()`. Handlers should call `throw_if_cancelled`; `progress` emits monotonic updates tied to the token. `log` applies the request's log threshold. Legacy `logging/setLevel` is not part of this protocol. Handler cancellation is cooperative: applications must make their own blocking work cancellable.

## Elicitation, sampling, roots and request state

The modern protocol returns `input_required` instead of making independent server-to-client RPC calls. `request_with_input` validates requests/responses, echoes opaque state only when supplied, uses fresh RPC IDs, and bounds rounds and the total time budget.

```cpp
mcp::InputHandlers handlers;
handlers.elicitation = [](const mcp::Json&) {
  return mcp::Json{{"action", "accept"}, {"content", {{"name", "Ada"}}}};
};
mcp::ClientOptions settings;
settings.capabilities = handlers.capabilities();
// Pass settings when constructing the client, then fulfill a workflow:
auto result = client.request_with_input("tools/call", {{"name", "ask-user"}, {"arguments", mcp::Json::object()}}, handlers);
```

Handlers receive method parameters. Form elicitation supports primitive fields, defaults and enums; the application renders the form, applies defaults and obtains user approval. Return `accept`, `decline` or `cancel` with matching content. URL elicitation uses `elicitation_url`, a callback returning an action, and `elicitation_url_enabled`; the application controls browser navigation and redirect completion. Do not claim an application identity policy is supplied by the SDK.

The offline example also implements `handlers.sampling` and `handlers.roots`. Sampling returns assistant content/model/stop reason; applications supply the actual model and approve context/tools. Enable `sampling_tools` or `sampling_context` only when the application handles them. Roots return approved `file://` workspace descriptors. The legacy `notifications/roots/list_changed` channel is not supported by the modern lifecycle; request the current roots through a new input workflow.

For multiround handlers, use `RequestContext::request_input` and the incoming `params().inputResponses` and `requestState`. State is application-owned: sign or otherwise integrity-protect it before trusting it, and bind security-sensitive state to the authenticated owner, operation, expiry and replay policy. The C++ conformance fixture includes signed state, missing inputs, multiple simultaneous inputs and multiple rounds.

## HTTP, stdio, authorization and limits

The four transport examples show HTTP and subprocess clients/servers. HTTP supports TLS, SSE response and subscription streams, connection limits and token providers. It is not the removed standalone legacy HTTP+SSE session transport. Set `allowed_origins`/`allowed_hosts` explicitly for the intended deployment. TLS certificate/key files and identity/token verification belong to the application.

Use `OAuthTransport` for bounded 401/403 authorization retries. It discovers protected-resource and authorization-server metadata, selects CIMD or dynamic registration, uses PKCE S256, verifies state and issuer, selects advertised token authentication, and combines previously granted and newly challenged scopes. Pre-registered credentials must be bound with `credential_issuer`. The application supplies the authorization callback and persistence policy. The callback must return the actual redirect parameters, not values reconstructed from the expected session.

Low-level `OAuthClient` exposes discovery, registration, authorization, completion, refresh, client credentials and enterprise token exchange. `ResourceAuthorization` publishes metadata and validates token claims through the application's verifier. Experimental DPoP/WIF and task/app conformance are not claimed by the 2026-07-28 Tier 1 score; their independent results stay visible.

`Limits`, `HttpServerOptions`, `HttpClientOptions` and `ClientOptions` bound bytes, connections, subscriptions, pages and input rounds. Memory resources must outlive allocations and be thread-safe when shared; see the API contract. `HttpServer` borrows `Server`, and clients retain their transport. Stop listeners before destroying servers and explicitly close owned subprocess transports when finished.

## Additional configuration examples

Enum/default form fields remain JSON Schema data for the application's UI:

```cpp
auto request = mcp::elicitation_form(
    "Choose a color",
    {{"type", "object"}, {"properties", {{"color", {{"type", "string"}, {"enum", {"red", "blue"}}, {"default", "blue"}}}}}, {"required", {"color"}}});
// In a handler: return context.request_input("color", request, signed_state).
```

A URL-mode callback can ask for an explicit action instead of automatically approving navigation:

```cpp
mcp::InputHandlers handlers;
handlers.elicitation_url_enabled = true;
handlers.elicitation = [](const mcp::Json& parameters) {
  if (parameters.value("mode", mcp::Json("form")) == "url") {
    std::cout << mcp::text(parameters["url"]) << "\naccept / decline / cancel: ";
    std::string action;
    if (!std::getline(std::cin, action))
      action = "cancel";
    return mcp::Json{{"action", action}};
  }
  return mcp::Json{{"action", "decline"}};
};
```

Tool header annotations are declared on primitive properties; the client mirrors values after obtaining the tool schema:

```cpp
mcp::Tool tool{"regional-query", "A regional query"};
tool.input_schema = {{"type", "object"}, {"properties", {{"region", {{"type", "string"}, {"x-mcp-header", "Region"}}}}}};
server.add_tool(tool, [](const mcp::Json& args, mcp::RequestContext&) { return mcp::text_result(mcp::text(args["region"])); });
client.list_tools();
auto result = client.call_tool("regional-query", {{"region", "eu-west"}});
```

For TLS, use certificates for the deployed hostname and keep verification enabled:

```cpp
mcp::HttpServerOptions listener_options;
listener_options.certificate_file = "server.crt";
listener_options.private_key_file = "server.key";
mcp::HttpServer listener(server, listener_options);

mcp::HttpClientOptions connection;
connection.url = "https://localhost:8443/mcp";
connection.ca_file = "trusted-ca.pem";
auto transport = std::make_shared<mcp::HttpTransport>(connection);
```

These fragments use the same `server` and `client` declarations as the complete example. For the TLS server, set `port = 8443` before construction and call `start`; certificate provisioning and trust-store deployment belong to the application.
