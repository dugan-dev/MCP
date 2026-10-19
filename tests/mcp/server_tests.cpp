#include <future>
#include <gtest/gtest.h>
#include <mcp/client.h>
#include <mcp/extensions.h>
#include <mcp/server.h>
#include <mutex>
#include <thread>

using namespace mcp;
namespace {
class LocalTransport : public Transport {
 public:
  Server& server;
  explicit LocalTransport(Server& value) : server(value) {}
  Json exchange(const Json& r, const RequestOptions& o, NotificationHandler n) override { return server.dispatch(r, o.stop, std::move(n)); }
  void close() noexcept override {}
};
Json call(Server& server, std::string_view name, Json arguments = Json::object(), Json caps = Json::object()) {
  return server.dispatch(make_request(1, "tools/call", {{"name", name}, {"arguments", std::move(arguments)}}, std::move(caps)));
}
}  // namespace
TEST(Server, DiscoveryIdentityCachingAndCapabilities) {
  Server server;
  auto result = server.dispatch(make_request(1, "server/discover"))["result"];
  EXPECT_EQ(result["resultType"], "complete");
  EXPECT_EQ(result["cacheScope"], "private");
  EXPECT_EQ(result["ttlMs"], 0);
  EXPECT_EQ(result["_meta"][server_info_key]["name"], "MCP");
  EXPECT_TRUE(result["capabilities"].contains("tools"));
  EXPECT_FALSE(result["capabilities"].contains("logging"));
  EXPECT_FALSE(result["capabilities"].contains("extensions"));
}
TEST(Server, ToolInputAndStructuredOutput) {
  Server server;
  server.add_tool(
      {"number", "", {{"type", "object"}, {"required", {"x"}}, {"properties", {{"x", {{"type", "integer"}}}}}}, Json{{"type", "integer"}}},
      [](const Json& args, RequestContext&) {
        auto r = text_result("number");
        r["structuredContent"] = args["x"];
        return r;
      });
  EXPECT_EQ(call(server, "number", {{"x", 3}})["result"]["structuredContent"], 3);
  EXPECT_EQ(call(server, "number", {{"x", "bad"}})["error"]["code"], -32602);
  EXPECT_EQ(call(server, "missing")["error"]["code"], -32602);
}
TEST(Server, ArbitraryStructuredContentAndOutputErrors) {
  Server server;
  server.add_tool({"array", "", {{"type", "object"}}, Json{{"type", "array"}}}, [](const Json&, RequestContext&) {
    auto result = text_result("array");
    result["structuredContent"] = Json::array({1, 2});
    return result;
  });
  EXPECT_TRUE(call(server, "array")["result"]["structuredContent"].is_array());
  server.add_tool({"broken", "", {{"type", "object"}}, Json{{"type", "number"}}},
                  [](const Json&, RequestContext&) { return text_result("missing output"); });
  EXPECT_EQ(call(server, "broken")["error"]["code"], -32603);
}
TEST(Server, ToolFailuresAreVisibleWithoutExceptionDetails) {
  Server server;
  server.add_tool({"fail"}, [](const Json&, RequestContext&) -> Json { throw std::runtime_error("secret token"); });
  const auto response = call(server, "fail");
  EXPECT_TRUE(response["result"]["isError"].get<bool>());
  EXPECT_EQ(serialize_json(response).find("secret"), std::string::npos);
}
TEST(Server, HandlerProtocolErrorRemainsProtocolError) {
  Server server;
  server.add_tool({"fail"}, [](const Json&, RequestContext&) -> Json { throw RpcError(42, "Denied"); });
  EXPECT_EQ(call(server, "fail")["error"]["code"], 42);
}
TEST(Server, InvalidHandlerOutputCannotEscape) {
  Server server;
  server.add_tool({"bad"}, [](const Json&, RequestContext&) { return Json{{"content", Json::array({{{"type", "image"}}})}}; });
  EXPECT_EQ(call(server, "bad")["error"]["code"], -32603);
}
TEST(Server, DeterministicPaginationAndStaleCursor) {
  ServerOptions options;
  options.limits.page_size = 2;
  Server server(options);
  for (std::string name : {"c", "a", "b"})
    server.add_tool({name}, [](const Json&, RequestContext&) { return text_result("ok"); });
  auto first = server.dispatch(make_request(1, "tools/list"))["result"];
  EXPECT_EQ(first["tools"][0]["name"], "a");
  EXPECT_EQ(first["tools"].size(), 2U);
  auto next = server.dispatch(make_request(2, "tools/list", {{"cursor", first["nextCursor"]}}))["result"];
  EXPECT_EQ(next["tools"][0]["name"], "c");
  EXPECT_FALSE(next.contains("nextCursor"));
  server.remove_tool("c");
  EXPECT_EQ(server.dispatch(make_request(3, "tools/list", {{"cursor", first["nextCursor"]}}))["error"]["code"], -32602);
}
TEST(Server, ClientCollectsAllPages) {
  ServerOptions options;
  options.limits.page_size = 1;
  Server server(options);
  for (std::string name : {"a", "b", "c"})
    server.add_tool({name}, [](const Json&, RequestContext&) { return text_result("ok"); });
  Client client(std::make_shared<LocalTransport>(server));
  EXPECT_EQ(client.list_tools().size(), 3U);
}
TEST(Server, ResourcesPromptsAndExplicitTemplateResolver) {
  Server server;
  server.add_resource({"example://a", "a"}, [](const Json&, RequestContext&) { return text_resource("example://a", "hello"); });
  server.add_resource_template({"example://{name}", "named"});
  server.set_resource_resolver([](const Json& params, RequestContext&) { return text_resource(text(params["uri"]), "resolved"); });
  server.add_prompt({"greet", "", Json::array({{{"name", "name"}, {"required", true}}})},
                    [](const Json& args, RequestContext&) { return prompt_result(text(args["name"])); });
  Client client(std::make_shared<LocalTransport>(server));
  EXPECT_EQ(client.read_resource("example://a")["contents"][0]["text"], "hello");
  EXPECT_EQ(client.read_resource("example://b")["contents"][0]["text"], "resolved");
  EXPECT_EQ(client.list_resource_templates().size(), 1U);
  EXPECT_THROW(client.get_prompt("greet"), RpcError);
  EXPECT_THROW(client.get_prompt("greet", {{"name", 1}}), RpcError);
  EXPECT_EQ(client.get_prompt("greet", {{"name", "world"}})["messages"][0]["content"]["text"], "world");
}
TEST(Server, CompletionBounds) {
  Server server;
  server.set_completion_handler([](const Json&, RequestContext&) { return Json{{"completion", {{"values", {"abc"}}, {"hasMore", false}}}}; });
  Client client(std::make_shared<LocalTransport>(server));
  EXPECT_EQ(client.complete({{"type", "ref/prompt"}, {"name", "x"}}, {{"name", "x"}, {"value", "a"}})["completion"]["values"][0], "abc");
  server.set_completion_handler([](const Json&, RequestContext&) { return Json{{"completion", {{"values", std::vector<std::string>(101, "x")}}}}; });
  EXPECT_THROW(client.complete({{"type", "ref/prompt"}, {"name", "x"}}, {{"name", "x"}, {"value", "a"}}), RpcError);
}
TEST(Server, ProgressOnlyWithTokenAndIncreasing) {
  Server server;
  server.add_tool({"work"}, [](const Json&, RequestContext& ctx) {
    ctx.progress(0, 2);
    ctx.progress(1, 2, "working");
    return text_result("ok");
  });
  std::vector<Json> notifications;
  auto r = make_request(1, "tools/call", {{"name", "work"}});
  server.dispatch(r, {}, [&](const Json& n) { notifications.push_back(n); });
  EXPECT_TRUE(notifications.empty());
  r["params"]["_meta"]["progressToken"] = "p";
  server.dispatch(r, {}, [&](const Json& n) { notifications.push_back(n); });
  ASSERT_EQ(notifications.size(), 2U);
  EXPECT_EQ(notifications[0]["params"]["progressToken"], "p");
}
TEST(Server, CapabilityCheckedPerRequestForElicitation) {
  Server server;
  server.add_tool({"confirm"}, [](const Json&, RequestContext& context) {
    return context.elicit("confirm", {{"message", "Continue?"}, {"requestedSchema", {{"type", "object"}, {"properties", Json::object()}}}}, "opaque");
  });
  auto denied = call(server, "confirm");
  EXPECT_EQ(denied["error"]["code"], -32021);
  EXPECT_TRUE(denied["error"]["data"]["requiredCapabilities"]["elicitation"].contains("form"));
  EXPECT_EQ(call(server, "confirm", Json::object(), {{"elicitation", Json::object()}})["result"]["resultType"], "input_required");
  EXPECT_EQ(call(server, "confirm", Json::object(), {{"elicitation", {{"url", Json::object()}}}})["error"]["code"], -32021);
}
TEST(Server, MultiRoundTripHasNoConnectionState) {
  Server server;
  server.add_tool({"confirm"}, [](const Json&, RequestContext& ctx) {
    if (ctx.params().contains("inputResponses"))
      return text_result(text(ctx.params()["inputResponses"]["confirm"]["action"]));
    return ctx.elicit("confirm", {{"message", "Continue?"}, {"requestedSchema", {{"type", "object"}, {"properties", Json::object()}}}}, "opaque");
  });
  ClientOptions options;
  options.capabilities = {{"elicitation", Json::object()}};
  Client client(std::make_shared<LocalTransport>(server), options);
  const auto result = client.request_with_input("tools/call", {{"name", "confirm"}}, [](const Json& request) {
    EXPECT_EQ(request["method"], "elicitation/create");
    return Json{{"action", "accept"}, {"content", Json::object()}};
  });
  EXPECT_EQ(result["content"][0]["text"], "accept");
}
TEST(Server, HandBuiltInputRequiredIsValidated) {
  Server server;
  server.add_tool({"bad"}, [](const Json&, RequestContext&) { return Json{{"resultType", "input_required"}}; });
  EXPECT_EQ(call(server, "bad")["error"]["code"], -32603);
}
TEST(Server, ExtensionsRequireExplicitDeclaration) {
  Server server;
  server.add_extension("com.example/test", Json::object(), "example/test", [](const Json&, RequestContext&) { return Json::object(); });
  EXPECT_TRUE(server.capabilities()["extensions"].contains("com.example/test"));
  auto r = make_request(1, "example/test", Json::object(), {{"extensions", {{"com.example/test", Json::object()}}}});
  EXPECT_EQ(server.dispatch(r)["result"]["resultType"], "complete");
  r["params"]["_meta"][capabilities_key] = Json::object();
  EXPECT_TRUE(server.dispatch(r).contains("error"));
}
TEST(Server, SubscriptionsAcknowledgeFilterAndClose) {
  Server server;
  std::vector<Json> messages;
  auto sub = server.subscribe(
      make_request("listen", "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}, {"resourceSubscriptions", {"example://a"}}}}}),
      [&](const Json& message) { messages.push_back(message); });
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0]["method"], "notifications/subscriptions/acknowledged");
  server.add_tool({"a"}, [](const Json&, RequestContext&) { return text_result("a"); });
  server.add_prompt({"p"}, [](const Json&, RequestContext&) { return prompt_result("p"); });
  server.resource_updated("example://b");
  server.resource_updated("example://a");
  ASSERT_EQ(messages.size(), 3U);
  EXPECT_EQ(messages[2]["params"]["_meta"][subscription_key], "listen");
  sub->close();
  sub->close();
  EXPECT_EQ(messages.size(), 4U);
  EXPECT_EQ(messages.back()["result"]["resultType"], "complete");
}
TEST(Server, TaskNotificationsHonorSubscribedIds) {
  Server server;
  namespace ext = mcp::extensions;
  auto store = std::make_shared<ext::MemoryTaskStore>();
  auto task =
      store->create(ext::make_task("issuer\nsubject", "task-a", ext::TaskStatus::working, std::chrono::minutes(1), std::chrono::milliseconds(1)));
  store->create(ext::make_task("issuer\nsubject", "task-b", ext::TaskStatus::working, std::chrono::minutes(1), std::chrono::milliseconds(1)));
  ext::install_tasks(server, store);
  const Json attributes = {{"iss", "issuer"}, {"sub", "subject"}};
  std::vector<Json> wanted, other;
  auto first = server.subscribe(
      make_request("first", "subscriptions/listen", {{"notifications", {{"taskIds", {"task-a"}}}}}, ext::tasks_capability()),
      [&](const Json& message) { wanted.push_back(message); }, attributes);
  auto second = server.subscribe(
      make_request("second", "subscriptions/listen", {{"notifications", {{"taskIds", {"task-b"}}}}}, ext::tasks_capability()),
      [&](const Json& message) { other.push_back(message); }, attributes);
  ext::transition_task(task, ext::TaskStatus::completed, {{"result", text_result("done")}});
  ASSERT_TRUE(store->compare_exchange(task.owner, "task-a", task.revision, task));
  server.notify("notifications/tasks", task.wire, task.owner);
  ASSERT_EQ(wanted.size(), 2U);
  ASSERT_EQ(other.size(), 1U);
  EXPECT_EQ(wanted.back()["method"], "notifications/tasks");
  EXPECT_EQ(wanted.back()["params"]["taskId"], "task-a");
  first->close();
  second->close();
}
TEST(Server, SubscriptionLimitAndBadFilter) {
  ServerOptions options;
  options.max_subscriptions = 1;
  Server server(options);
  auto r = make_request(1, "subscriptions/listen", {{"notifications", Json::object()}});
  auto sub = server.subscribe(r, [](const Json&) {});
  EXPECT_THROW(server.subscribe(r, [](const Json&) {}), RpcError);
  sub->close();
  r["params"]["notifications"]["toolsListChanged"] = "bad";
  EXPECT_THROW(server.subscribe(r, [](const Json&) {}), RpcError);
}
TEST(Server, HandlerRunsOutsideRegistryLock) {
  Server server;
  server.add_tool({"a"}, [&](const Json&, RequestContext&) {
    server.add_tool({"b"}, [](const Json&, RequestContext&) { return text_result("b"); });
    return text_result("a");
  });
  EXPECT_TRUE(call(server, "a").contains("result"));
  EXPECT_TRUE(call(server, "b").contains("result"));
}
TEST(Server, ConcurrentRegistrationAndCallsRetainEntries) {
  Server server;
  server.add_tool({"a"}, [](const Json&, RequestContext&) { return text_result("old"); });
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&] {
      for (int i = 0; i < 100; ++i)
        EXPECT_TRUE(call(server, "a").contains("result"));
    });
  for (int i = 0; i < 50; ++i)
    server.add_tool({"a"}, [](const Json&, RequestContext&) { return text_result("new"); });
  for (auto& thread : threads)
    thread.join();
}
TEST(Server, CancellationReachesHandler) {
  Server server;
  std::promise<void> entered;
  server.add_tool({"wait"}, [&](const Json&, RequestContext& ctx) {
    entered.set_value();
    while (!ctx.stop_token().stop_requested())
      std::this_thread::yield();
    ctx.throw_if_cancelled();
    return text_result("never");
  });
  std::stop_source source;
  auto result =
      std::async(std::launch::async, [&] { return server.dispatch(make_request(1, "tools/call", {{"name", "wait"}}), source.get_token()); });
  entered.get_future().wait();
  source.request_stop();
  EXPECT_EQ(result.get()["error"]["code"], 1001);
}
