#include <mcp/all.h>
#include <curl/curl.h>
#include <cstdlib>
#include <iostream>

namespace {
using mcp::Json;
Json callback_parameters(const std::string& url) {
  const auto question = url.find('?');
  if (question == std::string::npos)
    throw std::runtime_error("Authorization redirect has no query");
  Json result = Json::object();
  auto* curl = curl_easy_init();
  if (!curl)
    throw std::bad_alloc();
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> owner(curl, curl_easy_cleanup);
  std::string_view query(url.data() + question + 1, url.size() - question - 1);
  while (!query.empty()) {
    const auto end = query.find('&');
    const auto field = query.substr(0, end);
    const auto equal = field.find('=');
    if (equal != std::string_view::npos) {
      auto decode = [&](std::string_view value) {
        int length = 0;
        auto* bytes = curl_easy_unescape(curl, value.data(), static_cast<int>(value.size()), &length);
        if (!bytes)
          throw std::bad_alloc();
        std::unique_ptr<char, decltype(&curl_free)> memory(bytes, curl_free);
        return std::string(bytes, length);
      };
      result[decode(field.substr(0, equal))] = decode(field.substr(equal + 1));
    }
    if (end == std::string_view::npos)
      break;
    query.remove_prefix(end + 1);
  }
  return result;
}
Json input_response(const Json& request) {
  const auto method = mcp::text(request["method"]);
  if (method == "roots/list")
    return {{"roots", Json::array({{{"uri", "file:///workspace"}, {"name", "Workspace"}}})}};
  if (method == "sampling/createMessage")
    return {{"role", "assistant"},
            {"model", "conformance-fixture"},
            {"stopReason", "endTurn"},
            {"content", {{"type", "text"}, {"text", "A sample response"}}}};
  Json values = Json::object();
  if (request["params"].contains("requestedSchema")) {
    const auto properties = request["params"]["requestedSchema"].value("properties", Json::object());
    for (auto it = properties.begin(); it != properties.end(); ++it) {
      auto type = it.value().value("type", Json("string"));
      if (type == "boolean")
        values[it.key()] = true;
      else if (type == "integer" || type == "number")
        values[it.key()] = 42;
      else
        values[it.key()] = it.key() == "name" ? "Alice" : "test";
    }
  }
  return {{"action", "accept"}, {"content", values}};
}
}  // namespace
int main(int argc, char** argv) {
  std::string scenario = std::getenv("MCP_CONFORMANCE_SCENARIO") ? std::getenv("MCP_CONFORMANCE_SCENARIO") : "tools_call";
  try {
    if (argc < 2)
      throw std::invalid_argument("Server URL required");
    const std::string url = argv[argc - 1];
    Json context = std::getenv("MCP_CONFORMANCE_CONTEXT") ? mcp::parse_json(std::getenv("MCP_CONFORMANCE_CONTEXT")) : Json::object();
    std::shared_ptr<mcp::Transport> transport;
    if (scenario.starts_with("auth/")) {
      mcp::OAuthTransportOptions options;
      options.http.url = url;
      options.oauth.resource = url;
      options.oauth.client_id = "https://conformance-test.local/client-metadata.json";
      options.oauth.redirect_uri = "http://localhost:3000/callback";
      if (context.contains("client_id"))
        options.oauth.client_id = std::string(mcp::text(context["client_id"]));
      if (context.contains("client_secret"))
        options.oauth.client_secret = std::string(mcp::text(context["client_secret"]));
      if (context.contains("issuer"))
        options.oauth.credential_issuer = std::string(mcp::text(context["issuer"]));
      const auto network = mcp::curl_auth_exchange();
      options.authorize = [network](const mcp::AuthorizationSession& session, const mcp::RequestOptions& controls) {
        auto response = network({session.url, "GET", {}, {}}, controls);
        if (response.status < 300 || response.status >= 400 || !response.headers.contains("location"))
          throw mcp::AuthError("invalid_redirect", "Expected authorization redirect");
        return callback_parameters(response.headers.at("location"));
      };
      transport = std::make_shared<mcp::OAuthTransport>(std::move(options));
    } else
      transport = std::make_shared<mcp::HttpTransport>(mcp::HttpClientOptions{url});
    mcp::ClientOptions settings;
    settings.capabilities = {
        {"roots", Json::object()}, {"sampling", Json::object()}, {"elicitation", {{"form", Json::object()}, {"url", Json::object()}}}};
    mcp::Client client(transport, settings);
    client.discover();
    auto tools = client.list_tools();
    if (scenario == "json-schema-2020-12-preservation") {
      for (const auto& tool : tools)
        if (mcp::text(tool["name"]) == "json_schema_2020_12_tool")
          client.call_tool("json_schema_echo", {{"schema", tool["inputSchema"]}});
    } else if (scenario == "sep-2322-client-request-state") {
      for (const char* name : {"test_mrtr_echo_state", "test_mrtr_no_state", "test_mrtr_unrelated", "test_mrtr_no_result_type"})
        client.request_with_input("tools/call", {{"name", name}, {"arguments", Json::object()}}, input_response);
    } else if (context.contains("toolCalls")) {
      for (const auto& call : context["toolCalls"])
        client.call_tool(mcp::text(call["name"]), call["arguments"]);
    } else if (scenario != "json-schema-ref-no-deref" && scenario != "json-schema-2020-12-preservation") {
      for (const auto& tool : tools) {
        const auto name = std::string(mcp::text(tool["name"]));
        Json args = Json::object();
        const auto properties = tool["inputSchema"].value("properties", Json::object());
        for (auto it = properties.begin(); it != properties.end(); ++it) {
          auto type = it.value().value("type", Json("string"));
          if (type == "integer" || type == "number")
            args[it.key()] = 1;
          else if (type == "boolean")
            args[it.key()] = true;
          else
            args[it.key()] = "test";
        }
        client.call_tool(name, args);
      }
    }
    if (scenario == "http-standard-headers") {
      for (const auto& resource : client.list_resources())
        client.read_resource(mcp::text(resource["uri"]));
      for (const auto& prompt : client.list_prompts())
        client.get_prompt(mcp::text(prompt["name"]));
    }
    client.close();
  } catch (const mcp::AuthError& error) {
    const bool expected = (scenario == "auth/resource-mismatch" && error.code() == "resource_mismatch") ||
                          ((scenario == "auth/iss-supported-missing" || scenario == "auth/iss-wrong-issuer" || scenario == "auth/iss-unexpected" ||
                            scenario == "auth/iss-normalized" || scenario == "auth/metadata-issuer-mismatch") &&
                           (error.code() == "issuer_mismatch" || error.code() == "missing_issuer"));
    std::cerr << error.code() << ": " << error.what() << '\n';
    return expected ? 0 : 1;
  } catch (const mcp::HttpAuthorizationError& error) {
    std::cerr << error.what() << '\n';
    return scenario == "auth/scope-retry-limit" ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
