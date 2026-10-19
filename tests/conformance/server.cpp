#include <mcp/all.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <atomic>
#include <iostream>
#include <thread>

namespace {
using mcp::Json;
constexpr auto image = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a0xkAAAAASUVORK5CYII=";
constexpr auto audio = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQAAAAA=";
Json media(const char* type) {
  return {{"type", type},
          {"data", std::string_view(type) == "image" ? image : audio},
          {"mimeType", std::string_view(type) == "image" ? "image/png" : "audio/wav"}};
}
Json result(Json content) {
  return {{"content", std::move(content)}};
}
Json elicitation(std::string field, const char* type = "string") {
  return mcp::elicitation_form("Conformance input", {{"type", "object"}, {"properties", {{field, {{"type", type}}}}}, {"required", {field}}});
}
class State {
  std::array<unsigned char, 32> key_{};
  std::string signature(const std::string& payload) const {
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    if (!HMAC(EVP_sha256(), key_.data(), static_cast<int>(key_.size()), reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), bytes,
              &size))
      throw std::runtime_error("Cannot sign workflow state");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < size; ++i) {
      out += hex[bytes[i] >> 4];
      out += hex[bytes[i] & 15];
    }
    return out;
  }

 public:
  State() {
    if (RAND_bytes(key_.data(), static_cast<int>(key_.size())) != 1)
      throw std::runtime_error("Cannot initialize workflow key");
  }
  std::string sign(const Json& value) const {
    auto data = mcp::serialize_json(value);
    return data + "." + signature(data);
  }
  Json read(const std::string& value) const {
    const auto at = value.rfind('.');
    if (at == std::string::npos)
      throw mcp::RpcError(-32602, "Invalid workflow state");
    const auto data = value.substr(0, at), expected = signature(data), actual = value.substr(at + 1);
    if (actual.size() != expected.size() || CRYPTO_memcmp(actual.data(), expected.data(), expected.size()))
      throw mcp::RpcError(-32602, "Invalid workflow signature");
    return mcp::parse_json(data);
  }
};
}  // namespace

int main(int argc, char** argv) {
  try {
    mcp::ServerOptions settings;
    settings.info = {{"name", "MCP conformance fixture"}, {"version", mcp::library_version}};
    settings.cache_ttl_ms = 1000;
    mcp::Server server(settings);
    auto tool = [&](const char* name, mcp::Handler handler) {
      server.add_tool({name, name}, std::move(handler));
    };
    tool("test_simple_text", [](const Json&, auto&) { return mcp::text_result("This is a simple text response for testing."); });
    tool("test_image_content", [](const Json&, auto&) { return result(Json::array({media("image")})); });
    tool("test_audio_content", [](const Json&, auto&) { return result(Json::array({media("audio")})); });
    tool("test_embedded_resource", [](const Json&, auto&) {
      return result(Json::array(
          {{{"type", "resource"}, {"resource", {{"uri", "test://embedded"}, {"mimeType", "text/plain"}, {"text", "Embedded resource content"}}}}}));
    });
    tool("test_multiple_content_types", [](const Json&, auto&) {
      return result(Json::array({{{"type", "text"}, {"text", "Multiple content types"}},
                                 media("image"),
                                 {{"type", "resource"}, {"resource", {{"uri", "test://embedded"}, {"text", "Embedded content"}}}}}));
    });
    tool("test_error_handling", [](const Json&, auto&) {
      auto r = mcp::text_result("Intentional tool error");
      r["isError"] = true;
      return r;
    });
    tool("test_tool_with_progress", [](const Json&, auto& ctx) {
      ctx.progress(0, 100);
      ctx.progress(50, 100);
      ctx.progress(100, 100);
      return mcp::text_result("Progress complete");
    });
    tool("test_logging_tool", [](const Json&, auto& ctx) {
      ctx.log("info", "Conformance log");
      return mcp::text_result("Logged");
    });
    tool("test_tool_with_logging", [](const Json&, auto& ctx) {
      ctx.log("info", "Conformance log");
      return mcp::text_result("Logged");
    });
    tool("test_missing_capability", [](const Json&, auto& ctx) {
      ctx.require_capability("sampling");
      return mcp::text_result("Success");
    });
    tool("test_streaming_elicitation", [](const Json&, auto& ctx) { return ctx.request_input("confirmation", elicitation("ok", "boolean")); });
    std::atomic<unsigned> changes{0};
    tool("test_trigger_tool_change", [&](const Json&, auto&) {
      server.add_tool({"dynamic-" + std::to_string(++changes), "Dynamic tool"}, [](const Json&, auto&) { return mcp::text_result("ok"); });
      return mcp::text_result("Changed");
    });
    tool("test_trigger_prompt_change", [&](const Json&, auto&) {
      server.add_prompt({"dynamic-" + std::to_string(++changes), "Dynamic prompt"}, [](const Json&, auto&) { return mcp::prompt_result("ok"); });
      return mcp::text_result("Changed");
    });
    server.add_resource({"test://static-text", "static-text", "Static text", "text/plain"}, [](const Json&, auto&) {
      return mcp::text_resource("test://static-text", "This is the content of the static text resource.");
    });
    server.add_resource({"test://static-binary", "static-binary", "Static binary", "image/png"}, [](const Json&, auto&) {
      return Json{{"contents", Json::array({{{"uri", "test://static-binary"}, {"mimeType", "image/png"}, {"blob", image}}})}};
    });
    server.add_resource({"test://watched-resource", "watched", "Watched resource", "text/plain"},
                        [](const Json&, auto&) { return mcp::text_resource("test://watched-resource", "Watched resource content"); });
    server.add_resource_template({"test://template/{id}/data", "template", "Template resource", "application/json"});
    server.set_resource_resolver([](const Json& params, auto&) {
      auto uri = std::string(mcp::text(params["uri"]));
      if (!uri.starts_with("test://template/") || !uri.ends_with("/data"))
        throw mcp::RpcError(-32602, "Resource not found", {{"uri", uri}});
      const auto id = uri.substr(std::string("test://template/").size(), uri.size() - std::string("test://template/").size() - 5);
      return mcp::text_resource(uri, mcp::serialize_json({{"id", id}, {"templateTest", true}, {"data", "Data for ID: " + id}}), "application/json");
    });
    server.add_prompt({"test_simple_prompt", "Simple prompt"},
                      [](const Json&, auto&) { return mcp::prompt_result("This is a simple prompt for testing."); });
    server.add_prompt({"test_prompt_with_arguments", "Prompt arguments",
                       Json::array({{{"name", "arg1"}, {"required", true}}, {{"name", "arg2"}, {"required", true}}})},
                      [](const Json& args, auto&) { return mcp::prompt_result(mcp::serialize_json(args)); });
    server.add_prompt(
        {"test_prompt_with_embedded_resource", "Embedded resource", Json::array({{{"name", "resourceUri"}, {"required", true}}})},
        [](const Json& args, auto&) {
          return Json{{"messages",
                       Json::array({{{"role", "user"},
                                     {"content",
                                      {{"type", "resource"},
                                       {"resource", {{"uri", args["resourceUri"]}, {"mimeType", "text/plain"}, {"text", "Embedded content"}}}}}}})}};
        });
    server.add_prompt({"test_prompt_with_image", "Image prompt"},
                      [](const Json&, auto&) { return Json{{"messages", Json::array({{{"role", "user"}, {"content", media("image")}}})}}; });
    server.set_completion_handler(
        [](const Json&, auto&) { return Json{{"completion", {{"values", Json::array()}, {"total", 0}, {"hasMore", false}}}}; });
    mcp::Tool rich_schema{"json_schema_2020_12_tool", "JSON Schema 2020-12 vocabulary"};
    rich_schema.input_schema = mcp::parse_json(R"({
      "$schema":"https://json-schema.org/draft/2020-12/schema",
      "type":"object",
      "$defs":{"address":{"$anchor":"Address","type":"object","properties":{"street":{"type":"string"}}}},
      "properties":{"name":{"type":"string"},"address":{"$ref":"#/$defs/address"},
                    "email":{"type":"string"},"phone":{"type":"string"},
                    "kind":{"type":"string"},"company":{"type":"string"}},
      "additionalProperties":false,
      "allOf":[{"anyOf":[{"required":["email"]},{"required":["phone"]}]}],
      "if":{"properties":{"kind":{"const":"business"}},"required":["kind"]},
      "then":{"required":["company"]},"else":{}
    })");
    server.add_tool(rich_schema, [](const Json&, auto&) { return mcp::text_result("Schema preserved"); });
    State states;
    auto workflow = [&](const std::string& kind, mcp::RequestContext& ctx) -> Json {
      const auto& params = ctx.params();
      const auto responses = params.value("inputResponses", Json::object());
      if (!responses.is_object())
        throw mcp::RpcError(-32602, "inputResponses must be an object");
      for (const auto& r : responses)
        if (!r.is_object())
          throw mcp::RpcError(-32602, "Input response must be an object");
      Json state = Json::object();
      if (params.contains("requestState"))
        state = states.read(std::string(mcp::text(params["requestState"])));
      auto content = [&](const char* key, const char* field) {
        return responses.at(key).at("content").at(field);
      };
      auto ask = [&](const char* key, Json request, Json next = Json::object()) {
        return ctx.request_input(key, std::move(request), states.sign(next));
      };
      if (kind == "elicitation") {
        if (responses.contains("user_name"))
          return mcp::text_result("Hello, " + std::string(mcp::text(content("user_name", "name"))) + "!");
        return ask("user_name", elicitation("name"));
      }
      if (kind == "sampling") {
        if (responses.contains("sample_request"))
          return mcp::text_result(mcp::serialize_json(responses["sample_request"]));
        return ask("sample_request",
                   mcp::sampling_request(
                       {{"messages", Json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", "What is the capital of France?"}}}}})},
                        {"maxTokens", 100}}));
      }
      if (kind == "list_roots") {
        if (responses.contains("roots_request"))
          return mcp::text_result(mcp::serialize_json(responses["roots_request"]));
        return ask("roots_request", mcp::roots_request());
      }
      if (kind == "request_state" || kind == "tampered_state") {
        if (responses.contains("confirm") && state.contains("kind") && mcp::text(state["kind"]) == kind)
          return mcp::text_result("state-ok: confirmed");
        return ask("confirm", elicitation("ok", "boolean"), {{"kind", kind}});
      }
      if (kind == "multi_round") {
        if (state.value("round", 0) == 2 && responses.contains("step2"))
          return mcp::text_result(std::string(mcp::text(state.at("name"))) + " " + std::string(mcp::text(content("step2", "color"))));
        if (state.value("round", 0) == 1 && responses.contains("step1"))
          return ask("step2", elicitation("color"), {{"round", 2}, {"name", content("step1", "name")}});
        return ask("step1", elicitation("name"), {{"round", 1}});
      }
      Json requests = Json::object();
      if (kind == "multiple_inputs") {
        if (responses.contains("user_name") && responses.contains("greeting") && responses.contains("client_roots"))
          return mcp::text_result(mcp::serialize_json(responses));
        requests["user_name"] = elicitation("name");
        requests["greeting"] = mcp::sampling_request(
            {{"messages", Json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", "Generate a greeting"}}}}})}, {"maxTokens", 50}});
        requests["client_roots"] = mcp::roots_request();
      } else {
        if (!responses.empty())
          return mcp::text_result(mcp::serialize_json(responses));
        if (ctx.capabilities().contains("elicitation"))
          requests["elicit_input"] = elicitation("value");
        if (ctx.capabilities().contains("sampling"))
          requests["sample_input"] = mcp::sampling_request(
              {{"messages", Json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", "Sample request"}}}}})}, {"maxTokens", 50}});
      }
      if (requests.empty())
        return mcp::text_result("No supported capabilities declared");
      return mcp::input_required(requests, states.sign({{"kind", kind}}));
    };
    for (const auto* kind :
         {"elicitation", "sampling", "list_roots", "request_state", "multiple_inputs", "multi_round", "tampered_state", "capabilities"}) {
      server.add_tool({std::string("test_input_required_result_") + kind, "Input workflow"},
                      [&, kind = std::string(kind)](const Json&, auto& ctx) { return workflow(kind, ctx); });
    }
    server.add_prompt({"test_input_required_result_prompt", "Input prompt"}, [&](const Json&, auto& ctx) {
      if (ctx.params().value("inputResponses", Json::object()).contains("confirm"))
        return mcp::prompt_result("Confirmed");
      return ctx.request_input("confirm", elicitation("ok", "boolean"));
    });
    mcp::HttpServerOptions http;
    http.port = argc > 1 ? static_cast<std::uint16_t>(std::stoul(argv[1])) : 0;
    if (http.port)
      http.allowed_origins = {"http://127.0.0.1:" + std::to_string(http.port), "http://localhost:" + std::to_string(http.port)};
    mcp::HttpServer listener(server, http);
    listener.start();
    std::cout << listener.port() << std::endl;
    std::string line;
    std::getline(std::cin, line);
    listener.stop();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
