#include <mcp/all.h>

#include <cassert>
#include <iostream>

namespace {
class LocalTransport final : public mcp::Transport {
  mcp::Server& server_;
  bool closed_ = false;

 public:
  LocalTransport(mcp::Server& server) : server_(server) {}
  mcp::Json exchange(const mcp::Json& request, const mcp::RequestOptions& options, mcp::NotificationHandler notify = {}) override {
    if (closed_)
      throw mcp::TransportError(mcp::TransportErrorKind::closed, "Closed");
    if (options.stop.stop_requested())
      throw mcp::TransportError(mcp::TransportErrorKind::cancelled, "Cancelled");
    return server_.dispatch(request, options.stop, std::move(notify));
  }
  void close() noexcept override { closed_ = true; }
};
}  // namespace

int main() {
  using mcp::Json;
  mcp::ServerOptions settings;
  settings.limits.page_size = 2;
  settings.cache_ttl_ms = 1000;
  mcp::Server server(settings);
  server.add_tool({"echo", "Echo a string", {{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}, {"required", {"message"}}}},
                  [](const Json& args, mcp::RequestContext& ctx) {
                    ctx.throw_if_cancelled();
                    ctx.progress(0, 1);
                    ctx.log("info", "Echo called", "example");
                    ctx.progress(1, 1);
                    return mcp::text_result(mcp::text(args["message"]));
                  });
  server.add_tool({"media", "Mixed content"}, [](const Json&, auto&) {
    return Json{
        {"content",
         Json::array({{{"type", "image"},
                       {"mimeType", "image/png"},
                       {"data", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a0xkAAAAASUVORK5CYII="}},
                      {{"type", "audio"}, {"mimeType", "audio/wav"}, {"data", "UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQAAAAA="}},
                      {{"type", "resource"}, {"resource", {{"uri", "example://text"}, {"text", "Embedded text"}}}}})}};
  });
  server.add_tool({"failure", "Application error"},
                  [](const Json&, auto&) { return mcp::text_result("The application rejected the operation", true); });
  for (const auto* method : {"elicitation/create", "sampling/createMessage", "roots/list"}) {
    const std::string name = method == std::string_view("elicitation/create") ? "ask-user"
                             : method == std::string_view("roots/list")       ? "ask-roots"
                                                                              : "ask-model";
    server.add_tool({name, "Input workflow"}, [method](const Json&, auto& ctx) {
      if (ctx.params().value("inputResponses", Json::object()).contains("answer"))
        return mcp::text_result(mcp::serialize_json(ctx.params()["inputResponses"]["answer"]));
      Json request;
      if (method == std::string_view("elicitation/create"))
        request = mcp::elicitation_form(
            "What is your name?", {{"type", "object"}, {"properties", {{"name", {{"type", "string"}, {"default", "Ada"}}}}}, {"required", {"name"}}});
      else if (method == std::string_view("roots/list"))
        request = mcp::roots_request();
      else
        request = mcp::sampling_request(
            {{"messages", Json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", "Say hello"}}}}})}, {"maxTokens", 64}});
      return ctx.request_input("answer", request, "application-state");
    });
  }
  server.add_resource({"example://text", "text", "Text resource", "text/plain"},
                      [](const Json&, auto&) { return mcp::text_resource("example://text", "Hello"); });
  server.add_resource({"example://binary", "binary", "Binary resource", "application/octet-stream"}, [](const Json&, auto&) {
    return Json{{"contents", Json::array({{{"uri", "example://binary"}, {"blob", "aGVsbG8="}, {"mimeType", "application/octet-stream"}}})}};
  });
  server.add_resource_template({"example://item/{id}", "item", "Item template", "text/plain"});
  server.set_resource_resolver([](const Json& request, auto&) {
    const auto uri = mcp::text(request["uri"]);
    if (!uri.starts_with("example://item/"))
      throw mcp::RpcError(-32602, "Unknown resource");
    return mcp::text_resource(uri, uri.substr(15));
  });
  server.add_prompt({"greet", "Greeting", Json::array({{{"name", "name"}, {"required", true}}})},
                    [](const Json& args, auto&) { return mcp::prompt_result("Hello, " + std::string(mcp::text(args["name"]))); });
  server.set_completion_handler(
      [](const Json&, auto&) { return Json{{"completion", {{"values", {"Ada", "Grace"}}, {"total", 2}, {"hasMore", false}}}}; });
  unsigned changes = 0;
  auto subscription = server.subscribe(
      mcp::make_request(1, "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}, {"resourceSubscriptions", {"example://text"}}}}}),
      [&](const Json&) { ++changes; });
  server.resource_updated("example://text");
  server.add_tool({"added", "Added after subscribing"}, [](const Json&, auto&) { return mcp::text_result("Added"); });
  assert(changes >= 3);
  subscription->close();

  mcp::InputHandlers handlers;
  handlers.elicitation = [](const Json&) {
    return Json{{"action", "accept"}, {"content", {{"name", "Ada"}}}};
  };
  handlers.sampling = [](const Json&) {
    return Json{{"role", "assistant"}, {"model", "application-model"}, {"stopReason", "endTurn"}, {"content", {{"type", "text"}, {"text", "Hello"}}}};
  };
  handlers.roots = [](const Json&) {
    return Json{{"roots", Json::array({{{"uri", "file:///workspace"}, {"name", "Workspace"}}})}};
  };
  mcp::ClientOptions options;
  options.capabilities = handlers.capabilities();
  mcp::Client client(std::make_shared<LocalTransport>(server), options);
  client.discover();
  assert(client.list_tools().size() >= 6);
  mcp::RequestOptions controls;
  controls.timeout = std::chrono::seconds(5);
  controls.meta = {{"progressToken", "example"}, {"io.modelcontextprotocol/logLevel", "info"}};
  unsigned notifications = 0;
  auto echoed = client.call_tool("echo", {{"message", "Hello"}}, controls, [&](const Json&) { ++notifications; });
  assert(echoed["content"][0]["text"] == "Hello" && notifications == 3);
  client.call_tool("media");
  assert(client.call_tool("failure")["isError"] == true);
  for (const char* name : {"ask-user", "ask-model", "ask-roots"})
    client.request_with_input("tools/call", {{"name", name}, {"arguments", Json::object()}}, handlers);
  client.list_resources();
  client.list_resource_templates();
  client.read_resource("example://text");
  client.read_resource("example://binary");
  client.read_resource("example://item/42");
  client.list_prompts();
  client.get_prompt("greet", {{"name", "Ada"}});
  client.complete({{"type", "ref/prompt"}, {"name", "greet"}}, {{"name", "name"}, {"value", "A"}});
  std::stop_source cancelled;
  cancelled.request_stop();
  controls.stop = cancelled.get_token();
  try {
    client.discover(controls);
    return 1;
  } catch (const mcp::TransportError&) {}
  client.close();
  std::cout << "Feature examples passed\n";
}
