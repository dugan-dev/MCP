#include <gtest/gtest.h>
#include <mcp/client.h>
using namespace mcp;
namespace {
struct Peer final : Transport {
    std::function<Json(const Json &)> reply;
    explicit Peer(std::function<Json(const Json &)> value) : reply(std::move(value)) {}
    Json exchange(const Json &request, const RequestOptions &, NotificationHandler) override {
        return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", reply(request)}};
    }
    void close() noexcept override {}
};
} // namespace
TEST(Client, InvalidDiscoveryAndMalformedResultsAreRejected) {
    Client invalid(std::make_shared<Peer>([](const Json &) {
        return Json{{"supportedVersions", {"old"}}, {"capabilities", Json::object()}};
    }));
    EXPECT_THROW(invalid.discover(), TransportError);
    Client result(std::make_shared<Peer>(
        [](const Json &) { return Json{{"resultType", "input_required"}, {"inputRequests", 1}}; }));
    EXPECT_THROW(result.request("tools/call"), TransportError);
}
TEST(Client, AggregationDetectsLoopsAndLimits) {
    auto peer = std::make_shared<Peer>([](const Json &) {
        return Json{
            {"tools", Json::array({{{"name", "a"}, {"inputSchema", {{"type", "object"}}}}})},
            {"nextCursor", "repeat"}};
    });
    Client client(peer);
    EXPECT_THROW(client.list_tools(), TransportError);
    ClientOptions options;
    options.max_list_items = 1;
    Client bounded(peer, options);
    try {
        bounded.list_tools();
        FAIL();
    } catch (const TransportError &error) {
        EXPECT_EQ(error.kind(), TransportErrorKind::limit);
    }
    options.max_list_bytes = 1;
    Client byte_limit(peer, options);
    EXPECT_THROW(byte_limit.list_tools(), TransportError);
}
TEST(Client, DeclaredOutputIsCheckedAgainstRemoteSchema) {
    auto peer = std::make_shared<Peer>([](const Json &request) {
        if (request["method"] == "tools/list")
            return Json{{"tools", Json::array({{{"name", "bad"},
                                                {"inputSchema", {{"type", "object"}}},
                                                {"outputSchema", {{"type", "integer"}}}}})}};
        auto result = text_result("bad");
        result["structuredContent"] = "invalid";
        return result;
    });
    Client client(peer);
    EXPECT_THROW(client.call_tool("bad"), TransportError);
}
TEST(Client, StateOnlyRetryNeedsNoInputHandler) {
    int count = 0;
    auto peer = std::make_shared<Peer>([&](const Json &request) {
        if (++count == 1)
            return input_required(Json::object(), "retry");
        EXPECT_EQ(request["params"]["requestState"], "retry");
        return text_result("complete");
    });
    Client client(peer);
    EXPECT_EQ(
        client.request_with_input("tools/call", {{"name", "state"}}, {})["content"][0]["text"],
        "complete");
}
TEST(Client, InputRequestsAndRoundsAreBounded) {
    auto peer = std::make_shared<Peer>(
        [](const Json &) { return input_required(Json::object(), "again"); });
    ClientOptions options;
    options.max_input_rounds = 2;
    Client client(peer, options);
    EXPECT_THROW(client.request_with_input("tools/call", {{"name", "retry"}}, {}), TransportError);
    auto inputs = std::make_shared<Peer>([](const Json &) {
        return input_required(
            {{"a", {{"method", "elicitation/create"}, {"params", Json::object()}}},
             {"b", {{"method", "elicitation/create"}, {"params", Json::object()}}}});
    });
    options.max_input_requests = 1;
    Client bounded(inputs, options);
    EXPECT_THROW(bounded.request_with_input("tools/call", {{"name", "retry"}},
                                            [](const Json &) { return Json::object(); }),
                 TransportError);
}
