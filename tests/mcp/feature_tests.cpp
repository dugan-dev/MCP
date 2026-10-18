#include <gtest/gtest.h>
#include <mcp/features.h>

using namespace mcp;

TEST(Features, FormAndUrlElicitationAreDistinctAndSchemaChecked) {
    auto form = elicitation_form("Choose", {{"type", "object"},
                                            {"properties", {{"name", {{"type", "string"}}}}},
                                            {"required", {"name"}}});
    EXPECT_EQ(form["method"], "elicitation/create");
    EXPECT_NO_THROW(validate_input_request(form, {{"elicitation", {{"form", Json::object()}}}}));
    EXPECT_THROW(validate_input_request(form, {{"elicitation", {{"url", Json::object()}}}}),
                 RpcError);
    EXPECT_NO_THROW(
        validate_input_response(form, {{"action", "accept"}, {"content", {{"name", "Ada"}}}}));
    EXPECT_THROW(validate_input_response(form, {{"action", "accept"}}), RpcError);
    EXPECT_THROW(validate_input_response(form, {{"action", "accept"}, {"content", {{"name", 1}}}}),
                 std::exception);
    EXPECT_NO_THROW(validate_input_response(form, {{"action", "decline"}}));

    auto url = elicitation_url("Authorize", "https://example.test/authorize");
    EXPECT_NO_THROW(validate_input_request(url, {{"elicitation", {{"url", Json::object()}}}}));
    EXPECT_NO_THROW(validate_input_response(url, {{"action", "accept"}}));
    EXPECT_THROW(validate_input_request(url, {{"elicitation", {{"form", Json::object()}}}}),
                 RpcError);
    EXPECT_THROW(elicitation_url("bad", "not an absolute URI"), std::exception);
}

TEST(Features, SamplingRespectsToolsAndContextCapabilities) {
    Json params = {
        {"messages",
         Json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", "hello"}}}}})},
        {"maxTokens", 128}};
    auto request = sampling_request(params);
    EXPECT_NO_THROW(validate_input_request(request, {{"sampling", Json::object()}}));
    EXPECT_THROW(validate_input_request(request, Json::object()), RpcError);
    EXPECT_NO_THROW(
        validate_input_response(request, {{"role", "assistant"},
                                          {"model", "test"},
                                          {"content", {{"type", "text"}, {"text", "yes"}}}}));
    EXPECT_THROW(
        validate_input_response(
            request, {{"role", "assistant"}, {"content", {{"type", "text"}, {"text", "yes"}}}}),
        std::exception);
    params["tools"] = Json::array();
    request = sampling_request(params);
    EXPECT_THROW(validate_input_request(request, {{"sampling", Json::object()}}), RpcError);
    EXPECT_NO_THROW(validate_input_request(request, {{"sampling", {{"tools", Json::object()}}}}));
    params.erase("tools");
    params["includeContext"] = "allServers";
    request = sampling_request(params);
    EXPECT_THROW(validate_input_request(request, {{"sampling", Json::object()}}), RpcError);
    EXPECT_NO_THROW(validate_input_request(request, {{"sampling", {{"context", Json::object()}}}}));
    params.erase("maxTokens");
    EXPECT_THROW(sampling_request(params), std::exception);
}

TEST(Features, RootsAndInputHandlersValidateActualResultShape) {
    const auto request = roots_request();
    EXPECT_NO_THROW(validate_input_request(request, {{"roots", Json::object()}}));
    EXPECT_NO_THROW(validate_input_response(
        request, {{"roots", Json::array({{{"uri", "file:///tmp/work"}, {"name", "work"}}})}}));
    EXPECT_THROW(validate_input_response(request, {{"roots", 1}}), std::exception);
    InputHandlers handlers;
    handlers.roots = [](const Json &) { return Json{{"roots", Json::array()}}; };
    EXPECT_EQ(handlers.capabilities(), (Json{{"roots", Json::object()}}));
    EXPECT_EQ(handlers(request)["roots"].size(), 0U);
    EXPECT_THROW(
        handlers(elicitation_form("x", {{"type", "object"}, {"properties", Json::object()}})),
        RpcError);
}

TEST(Features, LogLevelOrderAndPerRequestFilter) {
    EXPECT_EQ(log_level_name(LogLevel::notice), "notice");
    EXPECT_EQ(parse_log_level("warning"), LogLevel::warning);
    EXPECT_THROW(parse_log_level("verbose"), RpcError);
    auto request = make_request(1, "tools/list");
    request["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "verbose";
    EXPECT_THROW(validate_request(request), RpcError);
    request["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "warning";
    EXPECT_NO_THROW(validate_request(request));
}

TEST(Features, RequestContextBuildsTypedInputAndRejectsMissingCapability) {
    Server server;
    server.add_tool({"ask"}, [](const Json &, RequestContext &context) {
        return context.request_input(
            "choice", elicitation_form("Name", {{"type", "object"},
                                                {"properties", {{"name", {{"type", "string"}}}}}}));
    });
    auto params = Json{{"name", "ask"}};
    auto missing = server.dispatch(make_request(1, "tools/call", params));
    ASSERT_TRUE(missing.contains("error"));
    EXPECT_EQ(missing["error"]["code"], static_cast<int>(ErrorCode::missing_capability));
    auto supported = server.dispatch(
        make_request(2, "tools/call", params, {{"elicitation", {{"form", Json::object()}}}}));
    ASSERT_TRUE(supported.contains("result")) << supported.dump();
    EXPECT_EQ(supported["result"]["resultType"], "input_required");
    EXPECT_EQ(supported["result"]["inputRequests"]["choice"]["method"], "elicitation/create");
}
