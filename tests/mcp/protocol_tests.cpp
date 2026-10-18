#include <gtest/gtest.h>
#include <mcp/server.h>

using namespace mcp;
TEST(Protocol, RequestContainsCompleteEnvelope) {
    auto r = make_request("a", "tools/list");
    EXPECT_EQ(text(r["params"]["_meta"][version_key]), protocol_version);
    EXPECT_TRUE(r["params"]["_meta"][capabilities_key].is_object());
    EXPECT_NO_THROW(validate_request(r));
}
TEST(Protocol, IntegralNumberIdsNormalizeWithoutPrecisionLoss) {
    EXPECT_TRUE(valid_id(parse_json("1.0")));
    EXPECT_TRUE(valid_id(parse_json("1e3")));
    EXPECT_FALSE(valid_id(parse_json("1e100")));
    Json response = {{"jsonrpc", "2.0"}, {"id", 1.0}, {"result", Json::object()}};
    EXPECT_NO_THROW(validate_response(response, 1));
    response["id"] = 9007199254740992.0;
    EXPECT_THROW(validate_response(response, Json(9007199254740993ULL)), TransportError);
}
class InvalidId : public testing::TestWithParam<Json> {};
TEST_P(InvalidId, Rejected) {
    EXPECT_FALSE(valid_id(GetParam()));
    EXPECT_THROW(make_request(GetParam(), "tools/list"), RpcError);
}
INSTANTIATE_TEST_SUITE_P(Wire, InvalidId,
                         testing::Values(Json(), Json(1.5), Json(true), Json::array(),
                                         Json::object()));
TEST(Protocol, IntegerAndStringIdsArePreserved) {
    Server server;
    for (const Json &id : {Json(-123), Json(std::uint64_t(123)), Json("id"), Json("")}) {
        auto response = server.dispatch(make_request(id, "server/discover"));
        EXPECT_EQ(response["id"], id);
        EXPECT_NO_THROW(validate_response(response, id));
    }
}
TEST(Protocol, UnknownVersionIncludesSupportedVersions) {
    Server server;
    auto r = make_request(1, "tools/list");
    r["params"]["_meta"][version_key] = "1900-01-01";
    auto response = server.dispatch(r);
    EXPECT_EQ(response["error"]["code"], -32022);
    EXPECT_EQ(text(response["error"]["data"]["supported"][0]), protocol_version);
    EXPECT_EQ(response["error"]["data"]["requested"], "1900-01-01");
}
TEST(Protocol, MetadataIsRequiredOnEachRequest) {
    Server server;
    EXPECT_TRUE(server.dispatch(make_request(1, "tools/list")).contains("result"));
    auto r = make_request(2, "tools/list");
    r["params"]["_meta"].erase(capabilities_key);
    EXPECT_EQ(server.dispatch(r)["error"]["code"], -32602);
    r["params"].erase("_meta");
    EXPECT_EQ(server.dispatch(r)["error"]["code"], -32602);
}
TEST(Protocol, BatchesAreRejected) {
    Server server;
    EXPECT_EQ(server.dispatch(Json::array({make_request(1, "tools/list")}))["error"]["code"],
              -32600);
}
TEST(Protocol, NotificationsNeverReceiveResponses) {
    Server server;
    EXPECT_TRUE(server.dispatch(make_notification("notifications/cancelled", {{"requestId", 1}}))
                    .is_null());
}
TEST(Protocol, ParseErrorsOmitUnreadableId) {
    Server server;
    auto response = server.dispatch(std::string_view("{"));
    EXPECT_EQ(response["error"]["code"], -32700);
    EXPECT_FALSE(response.contains("id"));
}
TEST(Protocol, LegacyHandshakeHasActionableError) {
    Server server;
    auto response = server.dispatch(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
    EXPECT_TRUE(response.contains("error"));
}
TEST(Protocol, ResponsesCannotHaveBothResultAndError) {
    Json response = {{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"result", {{"resultType", "complete"}}},
                     {"error", {{"code", -32603}, {"message", "oops"}}}};
    EXPECT_THROW(validate_response(response, 1), TransportError);
    response.erase("error");
    EXPECT_NO_THROW(validate_response(response, 1));
    EXPECT_THROW(validate_response(response, 2), TransportError);
}
TEST(Protocol, ExtensionResultTypeIsOpenAndInvalidInputRequiredIsRejected) {
    Json response = {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"resultType", "unknown"}}}};
    EXPECT_NO_THROW(validate_response(response, 1));
    response["result"]["resultType"] = "input_required";
    EXPECT_THROW(validate_response(response, 1), TransportError);
    response["result"]["requestState"] = "opaque";
    EXPECT_NO_THROW(validate_response(response, 1));
}
