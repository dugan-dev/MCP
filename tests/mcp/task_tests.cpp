#include <mcp/extensions.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory_resource>
#include <limits>
#include <thread>
#include <type_traits>

using namespace mcp;
using namespace mcp::extensions;
using namespace std::chrono_literals;

namespace {
class CountTaskResource final : public std::pmr::memory_resource {
 public:
  std::atomic<std::size_t> live{0};

 private:
  void* do_allocate(std::size_t size, std::size_t alignment) override {
    auto* value = std::pmr::new_delete_resource()->allocate(size, alignment);
    live += size;
    return value;
  }
  void do_deallocate(void* value, std::size_t size, std::size_t alignment) override {
    live -= size;
    std::pmr::new_delete_resource()->deallocate(value, size, alignment);
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};
template <class Record>
Json retained_inputs(const Record& record) {
  if constexpr (requires { record.input_responses; })
    return record.input_responses;
  else
    return Json();
}
template <class Store>
std::unique_ptr<Store> bounded_store(std::size_t maximum) {
  if constexpr (std::is_constructible_v<Store, std::size_t>)
    return std::make_unique<Store>(maximum);
  else
    return std::make_unique<Store>();
}
Json task_subscription(std::string_view id, Json capabilities = tasks_capability()) {
  return make_request(10, "subscriptions/listen", {{"notifications", {{"taskIds", {id}}}}}, std::move(capabilities));
}
}  // namespace

TEST(ReviewTasks, SubscriptionWithoutTrustedIdentityFailsClosed) {
  auto store = std::make_shared<MemoryTaskStore>();
  store->create(make_task("issuer\nalice", "secret", TaskStatus::working, 1min, 1ms));
  Server server;
  install_tasks(server, store);
  int delivered = 0;
  EXPECT_THROW(server.subscribe(task_subscription("secret"), [&](const Json&) { ++delivered; }), RpcError);
  EXPECT_EQ(delivered, 0);
}

TEST(ReviewTasks, SubscriptionCannotSpoofIdentityInRequestMetadata) {
  auto store = std::make_shared<MemoryTaskStore>();
  store->create(make_task("issuer\nalice", "secret", TaskStatus::working, 1min, 1ms));
  Server server;
  install_tasks(server, store);
  auto request = task_subscription("secret");
  request["params"]["_meta"]["iss"] = "issuer";
  request["params"]["_meta"]["sub"] = "alice";
  EXPECT_THROW(server.subscribe(request, [](const Json&) {}), RpcError);
}

TEST(ReviewTasks, SubscriptionRequiresNegotiatedTasksAndInstalledSupport) {
  Server server;
  EXPECT_THROW(server.subscribe(task_subscription("secret"), [](const Json&) {}), RpcError);
  auto store = std::make_shared<MemoryTaskStore>();
  store->create(make_task("issuer\nalice", "secret", TaskStatus::working, 1min, 1ms));
  install_tasks(server, store);
  EXPECT_THROW(server.subscribe(task_subscription("secret", Json::object()), [](const Json&) {}), RpcError);
}

TEST(ReviewTasks, AcceptedInputsSurviveAtomicStoreUpdateWithoutAppearingOnWire) {
  auto store = std::make_shared<MemoryTaskStore>();
  auto task = make_task("issuer\nalice", "input", TaskStatus::working, 1min, 1ms);
  transition_task(
      task, TaskStatus::input_required,
      {{"inputRequests",
        {{"name",
          {{"method", "elicitation/create"}, {"params", {{"mode", "form"}, {"message", "Name?"}, {"requestedSchema", {{"type", "object"}}}}}}}}}});
  store->create(task);
  Server server;
  install_tasks(server, store);
  const Json inputs = {{"name", {{"action", "accept"}, {"content", {{"name", "Ada"}}}}}};
  DispatchOptions options;
  options.attributes = {{"iss", "issuer"}, {"sub", "alice"}};
  auto response = server.dispatch(make_request(1, "tasks/update", {{"taskId", "input"}, {"inputResponses", inputs}}, tasks_capability()), options);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  auto stored = store->get("issuer\nalice", "input");
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->wire["status"], "working");
  EXPECT_EQ(retained_inputs(*stored), inputs);
  EXPECT_EQ(serialize_json(task_result(*stored)).find("Ada"), std::string::npos);
  EXPECT_FALSE(stored->wire.contains("inputResponses"));
}

TEST(ReviewTasks, ExpiredRecordsAreReclaimedOnUnrelatedStoreActivity) {
  auto memory = std::make_shared<CountTaskResource>();
  JsonScope scope(memory);
  MemoryTaskStore store;
  for (int index = 0; index < 20; ++index)
    store.create(make_task("tenant", std::to_string(index), TaskStatus::working, 10ms, 1ms, std::string(4096, 'x')));
  ASSERT_GT(memory->live.load(), 0u);
  std::this_thread::sleep_for(30ms);
  EXPECT_FALSE(store.get("other", "unrelated"));
  EXPECT_EQ(memory->live.load(), 0u);
}

TEST(ReviewTasks, LiveTaskCapacityIsBoundedAndExpiredSlotsCanBeReused) {
  auto store = bounded_store<MemoryTaskStore>(2);
  store->create(make_task("tenant", "a", TaskStatus::working, 10ms, 1ms));
  store->create(make_task("tenant", "b", TaskStatus::working, 10ms, 1ms));
  EXPECT_THROW(store->create(make_task("tenant", "c", TaskStatus::working, 1min, 1ms)), std::length_error);
  std::this_thread::sleep_for(30ms);
  EXPECT_NO_THROW(store->create(make_task("tenant", "c", TaskStatus::working, 1min, 1ms)));
}

TEST(ReviewTasks, TrustedOwnersAreIsolatedEvenWhenTaskIdsCollide) {
  auto store = std::make_shared<MemoryTaskStore>();
  auto alice = store->create(make_task("issuer\nalice", "shared", TaskStatus::working, 1min, 1ms));
  auto bob = store->create(make_task("issuer\nbob", "shared", TaskStatus::working, 1min, 1ms));
  Server server;
  install_tasks(server, store);
  std::vector<Json> alice_events, bob_events;
  auto a =
      server.subscribe(task_subscription("shared"), [&](const Json& event) { alice_events.push_back(event); }, {{"iss", "issuer"}, {"sub", "alice"}});
  auto b =
      server.subscribe(task_subscription("shared"), [&](const Json& event) { bob_events.push_back(event); }, {{"iss", "issuer"}, {"sub", "bob"}});
  server.notify("notifications/tasks", alice.wire, alice.owner);
  EXPECT_EQ(alice_events.size(), 2u);
  EXPECT_EQ(bob_events.size(), 1u);
  server.notify("notifications/tasks", bob.wire, bob.owner);
  EXPECT_EQ(alice_events.size(), 2u);
  EXPECT_EQ(bob_events.size(), 2u);
  EXPECT_THROW(server.notify("notifications/tasks", alice.wire), std::invalid_argument);
  EXPECT_EQ(alice_events.size(), 2u);
  EXPECT_EQ(bob_events.size(), 2u);
}

TEST(ReviewTasks, OtherTrustedPrincipalCannotSubscribeToTask) {
  auto store = std::make_shared<MemoryTaskStore>();
  store->create(make_task("issuer\nalice", "secret", TaskStatus::working, 1min, 1ms));
  Server server;
  install_tasks(server, store);
  int events = 0;
  EXPECT_THROW(server.subscribe(task_subscription("secret"), [&](const Json&) { ++events; }, {{"iss", "issuer"}, {"sub", "bob"}}), RpcError);
  EXPECT_EQ(events, 0);
}

TEST(ReviewTasks, ExpiredAndRevokedSubscriptionsNeverReceiveTaskData) {
  auto store = std::make_shared<MemoryTaskStore>();
  auto task = store->create(make_task("issuer\nalice", "secret", TaskStatus::working, 20ms, 1ms));
  Server server;
  install_tasks(server, store);
  int events = 0;
  auto sub = server.subscribe(task_subscription("secret"), [&](const Json&) { ++events; }, {{"iss", "issuer"}, {"sub", "alice"}});
  ASSERT_EQ(events, 1);
  std::this_thread::sleep_for(30ms);
  server.notify("notifications/tasks", task.wire, task.owner);
  EXPECT_EQ(events, 1);
  auto replacement = store->create(make_task("issuer\nbob", "secret", TaskStatus::working, 1min, 1ms));
  server.notify("notifications/tasks", replacement.wire, replacement.owner);
  EXPECT_EQ(events, 1);
}

TEST(ReviewTasks, PartialInputsAndInvalidUpdatesRetainAcceptedValues) {
  auto task = make_task("tenant", "input", TaskStatus::working, 1min, 1ms);
  transition_task(
      task, TaskStatus::input_required,
      {{"inputRequests",
        {{"one", {{"method", "roots/list"}, {"params", Json::object()}}}, {"two", {{"method", "roots/list"}, {"params", Json::object()}}}}}});
  const Json answer = {{"roots", Json::array({{{"uri", "file:///first"}}})}};
  accept_task_inputs(task, {{"one", answer}, {"unknown", {{"roots", Json::array()}}}});
  EXPECT_EQ(task.input_responses, Json({{"one", answer}}));
  EXPECT_EQ(task.wire["status"], "input_required");
  const auto wire = task.wire;
  const auto inputs = task.input_responses;
  EXPECT_THROW(accept_task_inputs(task, {{"two", {{"roots", "bad"}}}}), RpcError);
  EXPECT_EQ(task.wire, wire);
  EXPECT_EQ(task.input_responses, inputs);
  accept_task_inputs(task, {{"one", {{"roots", Json::array()}}}, {"two", {{"roots", Json::array()}}}});
  EXPECT_EQ(task.input_responses["one"], answer);
  EXPECT_TRUE(task.input_responses.contains("two"));
  EXPECT_FALSE(task.input_responses.contains("unknown"));
  EXPECT_EQ(task.wire["status"], "working");
  EXPECT_EQ(serialize_json(task_result(task)).find("file:///first"), std::string::npos);
}

TEST(ReviewTasks, CasConflictDoesNotCommitRejectedInputValues) {
  MemoryTaskStore store;
  auto task = make_task("tenant", "input", TaskStatus::working, 1min, 1ms);
  transition_task(task, TaskStatus::input_required, {{"inputRequests", {{"one", {{"method", "roots/list"}, {"params", Json::object()}}}}}});
  task = store.create(task);
  auto first = task;
  accept_task_inputs(first, {{"one", {{"roots", Json::array({{{"uri", "file:///winner"}}})}}}});
  ASSERT_TRUE(store.compare_exchange("tenant", "input", task.revision, first));
  auto second = task;
  accept_task_inputs(second, {{"one", {{"roots", Json::array({{{"uri", "file:///loser"}}})}}}});
  EXPECT_FALSE(store.compare_exchange("tenant", "input", task.revision, second));
  EXPECT_EQ(store.get("tenant", "input")->input_responses, first.input_responses);
}

TEST(ReviewTasks, ExplicitMaintenancePreservesUnlimitedTasksAndUpdatedExpiry) {
  EXPECT_THROW(MemoryTaskStore(0), std::invalid_argument);
  MemoryTaskStore store(4);
  auto task = store.create(make_task("tenant", "finite", TaskStatus::working, 60ms, 1ms));
  store.create(make_task("tenant", "unlimited", TaskStatus::working, -1ms, 1ms));
  task.wire["ttlMs"] = 500;
  ASSERT_TRUE(store.compare_exchange("tenant", "finite", task.revision, task));
  std::this_thread::sleep_for(70ms);
  EXPECT_EQ(store.prune_expired(), 0u);
  ASSERT_TRUE(store.get("tenant", "finite"));
  auto finite = *store.get("tenant", "finite");
  finite.wire["ttlMs"] = 0;
  ASSERT_TRUE(store.compare_exchange("tenant", "finite", finite.revision, finite));
  EXPECT_EQ(store.prune_expired(), 1u);
  EXPECT_FALSE(store.get("tenant", "finite"));
  EXPECT_TRUE(store.get("tenant", "unlimited"));
}

TEST(ReviewTasks, LegacyAggregateInitializationStillSetsCancellationFlag) {
  TaskRecord record{"tenant", 1, Json::object(), {}, true};
  EXPECT_TRUE(record.cancel_requested);
  EXPECT_TRUE(record.input_responses.is_object());
  EXPECT_TRUE(record.input_responses.empty());
}

namespace {
class CompletedTaskPeer final : public Transport {
 public:
  int calls = 0;
  Json exchange(const Json& request, const RequestOptions&, NotificationHandler) override {
    ++calls;
    auto task = make_task("tenant", "done", TaskStatus::completed, 1min, 0ms);
    task.wire["result"] = text_result("done");
    return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", task_result(task)}};
  }
  void close() noexcept override {}
};
class WaitingTaskPeer final : public Transport {
 public:
  int updates = 0;
  Json exchange(const Json& request, const RequestOptions&, NotificationHandler) override {
    if (request["method"] == "tasks/update") {
      ++updates;
      return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", Json::object()}};
    }
    auto task = make_task("tenant", "wait", TaskStatus::working, 1min, 0ms);
    transition_task(task, TaskStatus::input_required, {{"inputRequests", {{"roots", {{"method", "roots/list"}, {"params", Json::object()}}}}}});
    return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", task_result(task)}};
  }
  void close() noexcept override {}
};
}  // namespace
TEST(ReviewTasks, MaximumTimeoutCanPollCompletedTaskWithoutOverflow) {
  auto peer = std::make_shared<CompletedTaskPeer>();
  Client client(peer);
  TaskClient tasks(client);
  RequestOptions options;
  options.timeout = std::chrono::milliseconds::max();
  EXPECT_EQ(tasks.run("done", {}, options)["content"][0]["text"], "done");
  EXPECT_EQ(peer->calls, 1);
}
TEST(ReviewTasks, InputCallbackDeadlinePreventsLateTaskUpdate) {
  auto peer = std::make_shared<WaitingTaskPeer>();
  Client client(peer);
  TaskClient tasks(client);
  RequestOptions options;
  options.timeout = 20ms;
  EXPECT_THROW(tasks.run(
                   "wait",
                   [](const Json&) {
                     std::this_thread::sleep_for(40ms);
                     return Json{{"roots", Json::array()}};
                   },
                   options),
               TransportError);
  EXPECT_EQ(peer->updates, 0);
}

namespace {
class TaskLoopback final : public Transport {
 public:
  Server& server;
  bool wire;
  TaskLoopback(Server& value, bool encoded) : server(value), wire(encoded) {}
  Json exchange(const Json& request, const RequestOptions&, NotificationHandler) override {
    auto result = server.dispatch(wire ? parse_json(serialize_json(request)) : request);
    return wire ? parse_json(serialize_json(result)) : result;
  }
  void close() noexcept override {}
};
class TaskBudgetPeer final : public Transport {
 public:
  bool immediate = false;
  std::stop_source* cancel_after_creation = nullptr;
  std::chrono::milliseconds creation_delay{30};
  std::vector<std::chrono::milliseconds> creation_budgets, poll_budgets;
  int calls = 0;
  Json exchange(const Json& request, const RequestOptions& options, NotificationHandler) override {
    ++calls;
    Json result;
    if (request["method"] == "tools/list") {
      result = {{"tools", Json::array({{{"name", "job"}, {"inputSchema", {{"type", "object"}}}}})}};
    } else if (request["method"] == "tools/call") {
      creation_budgets.push_back(options.timeout);
      std::this_thread::sleep_for(creation_delay);
      if (cancel_after_creation)
        cancel_after_creation->request_stop();
      result = immediate ? text_result("done") : create_task_result(make_task("tenant", "job", TaskStatus::working, 1min, 0ms));
    } else {
      EXPECT_EQ(request["method"], "tasks/get");
      poll_budgets.push_back(options.timeout);
      auto task = make_task("tenant", "job", TaskStatus::completed, 1min, 0ms);
      task.wire["result"] = text_result("done");
      result = task_result(task);
    }
    return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", std::move(result)}};
  }
  void close() noexcept override {}
};
}  // namespace
TEST(ReviewTasks, NativeTaskCreationAcceptsSignedFiniteAndUnlimitedTtl) {
  for (const bool wire : {false, true}) {
    for (const auto ttl : {60000ms, 0ms, -1ms}) {
      SCOPED_TRACE(std::string(wire ? "wire" : "native") + " ttl=" + std::to_string(ttl.count()));
      Server server;
      server.add_tool({"job"},
                      [ttl](const Json&, RequestContext&) { return create_task_result(make_task("tenant", "job", TaskStatus::working, ttl, 0ms)); });
      ClientOptions options;
      options.capabilities = tasks_capability();
      Client client(std::make_shared<TaskLoopback>(server, wire), options);
      Json result;
      ASSERT_NO_THROW(result = client.call_tool("job"));
      EXPECT_EQ(result["resultType"], "task");
      if (ttl < 0ms)
        EXPECT_TRUE(result["ttlMs"].is_null());
      else
        EXPECT_EQ(result["ttlMs"].get<std::int64_t>(), ttl.count());
    }
  }
}
TEST(ReviewTasks, TaskCreationRejectsMalformedAndOverflowedFields) {
  auto valid = create_task_result(make_task("tenant", "job", TaskStatus::working, 1min, 0ms));
  std::vector<Json> invalid;
  for (const Json& ttl : {Json(-1), Json(std::numeric_limits<std::uint64_t>::max())}) {
    auto result = valid;
    result["ttlMs"] = ttl;
    invalid.push_back(std::move(result));
  }
  for (const auto field : {"status", "createdAt", "lastUpdatedAt"}) {
    auto result = valid;
    result[field] = "invalid";
    invalid.push_back(std::move(result));
  }
  auto bad_poll = valid;
  bad_poll["pollIntervalMs"] = -1;
  invalid.push_back(std::move(bad_poll));
  for (const auto& result : invalid) {
    SCOPED_TRACE(serialize_json(result));
    Server server;
    server.add_tool({"job"}, [result](const Json&, RequestContext&) { return result; });
    auto response = server.dispatch(make_request(1, "tools/call", {{"name", "job"}}, tasks_capability()));
    EXPECT_TRUE(response.contains("error")) << response.dump();
  }
}
TEST(ReviewTasks, RunToolCarriesShrinkingCreationBudgetIntoPolling) {
  auto peer = std::make_shared<TaskBudgetPeer>();
  ClientOptions config;
  config.capabilities = tasks_capability();
  Client client(peer, config);
  TaskClient tasks(client);
  RequestOptions options;
  options.timeout = 400ms;
  EXPECT_EQ(tasks.run_tool("job", Json::object(), {}, options)["content"][0]["text"], "done");
  ASSERT_EQ(peer->creation_budgets.size(), 1u);
  ASSERT_EQ(peer->poll_budgets.size(), 1u);
  EXPECT_GT(peer->poll_budgets[0], 0ms);
  EXPECT_LE(peer->poll_budgets[0], peer->creation_budgets[0] - 20ms);
}
TEST(ReviewTasks, RunToolRejectsCreationThatConsumedOriginalDeadline) {
  for (const bool immediate : {false, true}) {
    auto peer = std::make_shared<TaskBudgetPeer>();
    peer->creation_delay = 60ms;
    peer->immediate = immediate;
    ClientOptions config;
    config.capabilities = tasks_capability();
    Client client(peer, config);
    TaskClient tasks(client);
    RequestOptions options;
    options.timeout = 20ms;
    try {
      (void)tasks.run_tool("job", Json::object(), {}, options);
      FAIL() << "Late creation returned successfully; immediate=" << immediate;
    } catch (const TransportError& error) {
      EXPECT_EQ(error.kind(), TransportErrorKind::timeout);
    }
    EXPECT_TRUE(peer->poll_budgets.empty());
  }
}
TEST(ReviewTasks, RunToolPreCancelledNeverStartsCreation) {
  auto peer = std::make_shared<TaskBudgetPeer>();
  ClientOptions config;
  config.capabilities = tasks_capability();
  Client client(peer, config);
  TaskClient tasks(client);
  std::stop_source stop;
  stop.request_stop();
  RequestOptions options;
  options.stop = stop.get_token();
  try {
    (void)tasks.run_tool("job", Json::object(), {}, options);
    FAIL();
  } catch (const TransportError& error) {
    EXPECT_EQ(error.kind(), TransportErrorKind::cancelled);
  }
  EXPECT_EQ(peer->calls, 0);
}

TEST(ReviewTasks, RunToolCancellationDuringCreationRejectsImmediateAndTaskResults) {
  for (const bool immediate : {false, true}) {
    auto peer = std::make_shared<TaskBudgetPeer>();
    std::stop_source stop;
    peer->immediate = immediate;
    peer->cancel_after_creation = &stop;
    ClientOptions config;
    config.capabilities = tasks_capability();
    Client client(peer, config);
    TaskClient tasks(client);
    RequestOptions options;
    options.stop = stop.get_token();
    try {
      (void)tasks.run_tool("job", Json::object(), {}, options);
      FAIL();
    } catch (const TransportError& error) {
      EXPECT_EQ(error.kind(), TransportErrorKind::cancelled);
    }
    EXPECT_TRUE(peer->poll_budgets.empty());
  }
}
