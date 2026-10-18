#include <atomic>
#include <mcp/mcp.h>
#include <gtest/gtest.h>
#include <memory_resource>
#include <thread>
#include <unimem/all.h>

using namespace mcp;
namespace {
struct ResourceCounts {
    std::atomic<std::size_t> allocations{0}, live_bytes{0};
};
class CountingResource final : public std::pmr::memory_resource {
  public:
    explicit CountingResource(std::shared_ptr<ResourceCounts> counts) : counts_(std::move(counts)) {}
  private:
    std::shared_ptr<ResourceCounts> counts_;
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto *value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++counts_->allocations;
        counts_->live_bytes += bytes;
        return value;
    }
    void do_deallocate(void *value, std::size_t bytes, std::size_t alignment) override {
        counts_->live_bytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }
};
}

TEST(Json, CallerOwnedGlobalSurvivesScopeOwnerAndCreatingThread) {
    auto memory = std::make_shared<unimem::Global>();
    std::weak_ptr<unimem::Global> owner = memory;
    JsonStorage resource(memory, memory->resource());
    Json escaped;
    std::thread create([resource, &escaped] {
        JsonScope scope(resource);
        escaped = {{"payload", std::string(8192, 'g')}, {"values", Json::array({1, 2, 3})}};
    });
    create.join();
    resource.reset();
    memory.reset();
    ASSERT_FALSE(owner.expired());
    std::thread destroy([value = std::move(escaped)]() mutable {
        EXPECT_EQ(text(value["payload"]).size(), 8192u);
        EXPECT_EQ(value["values"], Json::array({1, 2, 3}));
        value = nullptr;
    });
    destroy.join();
    EXPECT_TRUE(owner.expired());
}

TEST(Json, UnscopedDefaultContextSurvivesCreatingThread) {
    Json escaped;
    std::thread create([&escaped] {
        escaped = {{"payload", std::string(8192, 'd')}, {"values", Json::array({4, 5, 6})}};
    });
    create.join();
    std::thread destroy([value = std::move(escaped)]() mutable {
        EXPECT_EQ(text(value["payload"]).size(), 8192u);
        EXPECT_EQ(value["values"], Json::array({4, 5, 6}));
        value["values"].push_back(7);
        EXPECT_EQ(value["values"][3], 7);
        value = nullptr;
    });
    destroy.join();
}

TEST(Server, SubscriptionSurvivesServerAndReleasesOwnersOnAnotherThread) {
    auto counts = std::make_shared<ResourceCounts>();
    auto resource = std::make_shared<CountingResource>(counts);
    std::weak_ptr<std::pmr::memory_resource> owner = resource;
    std::shared_ptr<Subscription> subscription;
    Json acknowledged;
    std::size_t notices = 0;
    {
        ServerOptions options;
        options.memory_resource = resource;
        Server server(std::move(options));
        subscription = server.subscribe(make_request(19, "subscriptions/listen",
            {{"notifications", {{"toolsListChanged", true}}}}),
            [&](const Json &notice) {
                ++notices;
                acknowledged = notice;
            });
    }
    resource.reset();
    ASSERT_FALSE(owner.expired());
    EXPECT_EQ(notices, 1u);
    ASSERT_EQ(acknowledged["method"], "notifications/subscriptions/acknowledged");
    std::thread destroy([subscription = std::move(subscription)]() mutable {
        subscription->close();
        subscription.reset();
    });
    destroy.join();
    EXPECT_EQ(notices, 1u);
    EXPECT_FALSE(owner.expired());
    acknowledged = nullptr;
    EXPECT_TRUE(owner.expired());
    EXPECT_EQ(counts->live_bytes.load(), 0u);
}

TEST(Server, DefaultSubscriptionWeakControlBlockOutlivesServer) {
    for (int i = 0; i < 32; ++i) {
        std::shared_ptr<Subscription> subscription;
        std::weak_ptr<std::pmr::memory_resource> owner;
        std::size_t notices = 0;
        {
            Server server;
            owner = server.options().memory_resource;
            subscription = server.subscribe(make_request(i, "subscriptions/listen",
                {{"notifications", {{"toolsListChanged", true}}}}),
                [&](const Json &) { ++notices; });
        }
        ASSERT_FALSE(owner.expired());
        std::thread destroy([subscription = std::move(subscription)]() mutable {
            subscription->close();
            subscription.reset();
        });
        destroy.join();
        EXPECT_EQ(notices, 1u);
        EXPECT_TRUE(owner.expired());
    }
}

TEST(Json, CustomResourceOwnerSurvivesNestedScopesAndCrossThreadDestruction) {
    auto outer_counts = std::make_shared<ResourceCounts>();
    auto inner_counts = std::make_shared<ResourceCounts>();
    auto outer = std::make_shared<CountingResource>(outer_counts);
    auto inner = std::make_shared<CountingResource>(inner_counts);
    std::weak_ptr<std::pmr::memory_resource> outer_owner = outer, inner_owner = inner;
    Json first, second, restored;
    {
        JsonScope outer_scope(outer);
        first = Json{{"payload", std::string(4096, 'a')}};
        {
            JsonScope inner_scope(inner);
            second = Json{{"payload", std::string(4096, 'b')}};
        }
        const auto inner_allocations = inner_counts->allocations.load();
        const auto outer_allocations = outer_counts->allocations.load();
        restored = Json{{"payload", std::string(4096, 'c')}};
        EXPECT_EQ(inner_counts->allocations.load(), inner_allocations);
        EXPECT_GT(outer_counts->allocations.load(), outer_allocations);
    }
    outer.reset();
    inner.reset();
    ASSERT_FALSE(outer_owner.expired());
    ASSERT_FALSE(inner_owner.expired());
    std::thread destroy([first = std::move(first), second = std::move(second),
                         restored = std::move(restored)]() mutable {
        first = second = restored = nullptr;
    });
    destroy.join();
    EXPECT_EQ(outer_counts->live_bytes.load(), 0u);
    EXPECT_EQ(inner_counts->live_bytes.load(), 0u);
    EXPECT_TRUE(outer_owner.expired());
    EXPECT_TRUE(inner_owner.expired());
}

TEST(Server, CustomResourceCoversHelpersAndSubscriptionClose) {
    auto selected_counts = std::make_shared<ResourceCounts>();
    auto ambient_counts = std::make_shared<ResourceCounts>();
    auto selected = std::make_shared<CountingResource>(selected_counts);
    auto ambient = std::make_shared<CountingResource>(ambient_counts);
    ServerOptions options;
    options.memory_resource = selected;
    Server server(options);
    server.add_tool({"echo", "", {{"type", "object"},
        {"description", std::string(256, 'x')}}},
        [](const Json &, RequestContext &) { return text_result("ok"); });
    auto request = make_request(1, "subscriptions/listen",
        {{"notifications", {{"toolsListChanged", true},
                             {"resourceSubscriptions", {"fixture://one"}}}}});
    std::size_t notices = 0;
    auto sub = server.subscribe(request, [&](const Json &) { ++notices; });
    {
        JsonScope ambient_scope(ambient);
        const auto before = ambient_counts->allocations.load();
        auto capabilities = server.capabilities();
        auto schema = server.tool_schema("echo");
        server.resource_updated("fixture://one");
        EXPECT_TRUE(server.remove_tool("echo"));
        sub->close();
        EXPECT_EQ(ambient_counts->allocations.load(), before);
        EXPECT_TRUE(capabilities.contains("tools"));
        EXPECT_EQ(schema["type"], "object");
    }
    EXPECT_EQ(notices, 4u); // acknowledgment, resource update, list update, graceful completion
}

TEST(Server, DefaultResourceResultsEscapeServerAndAmbientScopeAcrossThreads) {
    auto ambient_counts = std::make_shared<ResourceCounts>();
    auto ambient = std::make_shared<CountingResource>(ambient_counts);
    std::weak_ptr<std::pmr::memory_resource> ambient_owner = ambient;
    Json discovery_request = make_request(1, "server/discover");
    Json call_request = make_request(2, "tools/call", {{"name", "escape"}});
    Tool tool{"escape", "", {{"type", "object"}}};
    ServerOptions server_options;
    Json discovery, result, capabilities;
    {
        Server server(std::move(server_options));
        server.add_tool(std::move(tool), [](const Json &, RequestContext &) {
            return text_result(std::string(4096, 'x'));
        });
        {
            JsonScope ambient_scope(ambient);
            const auto before = ambient_counts->allocations.load();
            discovery = server.dispatch(discovery_request);
            result = server.dispatch(call_request);
            capabilities = server.capabilities();
            EXPECT_EQ(ambient_counts->allocations.load(), before);
            ASSERT_TRUE(discovery.contains("result"));
            EXPECT_EQ(discovery["result"]["supportedVersions"], Json::array({protocol_version}));
            EXPECT_EQ(discovery["result"]["capabilities"], capabilities);
        }
    }
    ambient.reset();
    EXPECT_TRUE(ambient_owner.expired());
    EXPECT_EQ(ambient_counts->live_bytes.load(), 0u);
    ASSERT_TRUE(result.contains("result"));
    EXPECT_EQ(result["result"]["_meta"][server_info_key]["name"], "MCP");
    std::thread release([result = std::move(result), discovery = std::move(discovery),
                         capabilities = std::move(capabilities)]() mutable {
        EXPECT_EQ(text(result["result"]["content"][0]["text"]).size(), 4096u);
        EXPECT_TRUE(capabilities.contains("tools"));
        result = discovery = capabilities = nullptr;
    });
    release.join();
}

TEST(Stdio, DirectExchangeUsesConfiguredResourceForCallbacks) {
    auto selected_counts = std::make_shared<ResourceCounts>();
    auto ambient_counts = std::make_shared<ResourceCounts>();
    auto selected = std::make_shared<CountingResource>(selected_counts);
    auto ambient = std::make_shared<CountingResource>(ambient_counts);
    ProcessOptions process;
    process.executable = MCP_TEST_PEER;
    process.arguments = {"logging"};
    process.memory_resource = selected;
    auto request = make_request(1, "tools/call", {{"name", "logged"},
        {"arguments", {{"label", "resource"}}},
        {"_meta", {{"io.modelcontextprotocol/logLevel", "info"}}}});
    RequestOptions options;
    std::size_t callbacks = 0;
    {
        JsonScope ambient_scope(ambient);
        const auto before = ambient_counts->allocations.load();
        StdioTransport transport(std::move(process));
        auto result = transport.exchange(request, options, [&](const Json &) {
            Json value = {{"payload", std::string(256, 'x')}};
            EXPECT_EQ(value["payload"].get<String>().size(), 256u);
            ++callbacks;
        });
        EXPECT_TRUE(result.contains("result"));
        EXPECT_EQ(ambient_counts->allocations.load(), before);
        transport.close();
    }
    EXPECT_EQ(callbacks, 2u);
}

TEST(Client, StdioResultAndNotificationsRetainOwnerAfterClientTransportAndThreadEnd) {
    auto counts = std::make_shared<ResourceCounts>();
    auto resource = std::make_shared<CountingResource>(counts);
    std::weak_ptr<std::pmr::memory_resource> owner = resource;
    Json result;
    std::vector<Json> notifications;
    std::exception_ptr failure;
    std::thread create([resource, &result, &notifications, &failure] {
        try {
            ProcessOptions process;
            process.executable = MCP_TEST_PEER;
            process.arguments = {"logging"};
            process.memory_resource = resource;
            ClientOptions client_options;
            client_options.memory_resource = resource;
            Client client(std::make_shared<StdioTransport>(std::move(process)), std::move(client_options));
            RequestOptions options;
            options.meta["io.modelcontextprotocol/logLevel"] = "info";
            result = client.call_tool("logged", {{"label", "escaped"}}, std::move(options),
                [&](const Json &notice) { notifications.push_back(notice); });
            client.close();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    create.join();
    ASSERT_FALSE(failure);
    resource.reset();
    ASSERT_FALSE(owner.expired());
    ASSERT_EQ(notifications.size(), 2u);
    EXPECT_EQ(result["content"][0]["text"], "escaped");
    std::thread destroy([result = std::move(result), notifications = std::move(notifications)]() mutable {
        EXPECT_EQ(result["content"][0]["text"], "escaped");
        EXPECT_EQ(notifications[0]["method"], "notifications/message");
        result = nullptr;
        notifications.clear();
    });
    destroy.join();
    EXPECT_TRUE(owner.expired());
    EXPECT_EQ(counts->live_bytes.load(), 0u);
}
