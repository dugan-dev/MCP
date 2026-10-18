#include "input_schemas.h"
#include <array>
#include <curl/curl.h>
#include <map>
#include <mcp/features.h>
#include <mcp/schema.h>

namespace mcp {
namespace {
const Schema &schema(std::string_view name) {
    static const auto schemas = [] {
        std::map<std::string_view, Schema, std::less<>> result;
        for (const auto &[key, value] : detail::input_schemas)
            result.emplace(key, Schema(parse_json(value), SchemaLimits{64, 20000, 1024 * 1024}));
        return result;
    }();
    return schemas.at(name);
}
constexpr std::array<std::string_view, 8> levels{"debug", "info",     "notice", "warning",
                                                 "error", "critical", "alert",  "emergency"};
void validate_url(std::string_view value) {
    if (!value.starts_with("https://") && !value.starts_with("http://"))
        throw RpcError(ErrorCode::invalid_params, "Elicitation URL must be absolute HTTP(S)");
    auto *url = curl_url();
    if (!url)
        throw std::bad_alloc();
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> owner(url, curl_url_cleanup);
    const std::string input(value);
    if (input.find('\0') != std::string::npos ||
        curl_url_set(url, CURLUPART_URL, input.c_str(), 0) != CURLUE_OK)
        throw RpcError(ErrorCode::invalid_params, "Invalid elicitation URL");
    char *host = nullptr;
    const auto code = curl_url_get(url, CURLUPART_HOST, &host, 0);
    if (code != CURLUE_OK || !host || !*host) {
        if (host)
            curl_free(host);
        throw RpcError(ErrorCode::invalid_params, "Elicitation URL needs a host");
    }
    curl_free(host);
}
std::string_view type(std::string_view method, bool response) {
    if (method == "elicitation/create")
        return response ? "ElicitResult" : "ElicitRequest";
    if (method == "sampling/createMessage")
        return response ? "CreateMessageResult" : "CreateMessageRequest";
    if (method == "roots/list")
        return response ? "ListRootsResult" : "ListRootsRequest";
    throw RpcError(ErrorCode::method_not_found, "Unknown input request method");
}
void sampling_semantics(const Json &value) {
    if (!value.contains("messages"))
        return;
    for (const auto &message : value["messages"]) {
        const auto &content = message["content"];
        if (!content.is_array())
            continue;
        bool tool_result = false, other = false;
        for (const auto &block : content) {
            if (block.value("type", Json()) == "tool_result")
                tool_result = true;
            else
                other = true;
        }
        if (tool_result && other)
            throw RpcError(ErrorCode::invalid_params,
                           "Tool results cannot mix with other sampling content");
    }
}
} // namespace
std::string_view log_level_name(LogLevel level) noexcept {
    const auto index = static_cast<std::size_t>(level);
    return index < levels.size() ? levels[index] : std::string_view{};
}
LogLevel parse_log_level(std::string_view value) {
    const auto it = std::find(levels.begin(), levels.end(), value);
    if (it == levels.end())
        throw RpcError(ErrorCode::invalid_params, "Invalid log level");
    return static_cast<LogLevel>(std::distance(levels.begin(), it));
}
Json elicitation_form(std::string_view message, Json definition) {
    Json request = {
        {"method", "elicitation/create"},
        {"params",
         {{"mode", "form"}, {"message", message}, {"requestedSchema", std::move(definition)}}}};
    schema("ElicitRequest").validate(request);
    return request;
}
Json elicitation_url(std::string_view message, std::string_view url) {
    validate_url(url);
    Json request = {{"method", "elicitation/create"},
                    {"params", {{"mode", "url"}, {"message", message}, {"url", url}}}};
    schema("ElicitRequest").validate(request);
    return request;
}
Json sampling_request(Json parameters) {
    Json request = {{"method", "sampling/createMessage"}, {"params", std::move(parameters)}};
    schema("CreateMessageRequest").validate(request);
    sampling_semantics(request["params"]);
    return request;
}
Json roots_request() {
    return {{"method", "roots/list"}, {"params", Json::object()}};
}
void validate_input_request(const Json &request, const Json &capabilities) {
    if (!request.is_object() || !request.contains("method") || !request["method"].is_string())
        throw RpcError(ErrorCode::invalid_params, "Invalid input request");
    const auto method = text(request["method"]);
    schema(type(method, false)).validate(request);
    const auto key = method == "elicitation/create" ? "elicitation"
                     : method == "roots/list"       ? "roots"
                                                    : "sampling";
    Json needed = {{key, Json::object()}};
    bool supported =
        capabilities.is_object() && capabilities.contains(key) && capabilities[key].is_object();
    const auto &parameters = request["params"];
    if (method == "elicitation/create") {
        const auto mode = parameters.value("mode", Json("form"));
        if (mode == "url")
            validate_url(text(parameters["url"]));
        needed[key][text(mode)] = Json::object();
        supported =
            supported &&
            (mode == "form" ? (capabilities[key].empty() || capabilities[key].contains("form"))
                            : capabilities[key].contains("url"));
    } else if (method == "sampling/createMessage") {
        sampling_semantics(parameters);
        if (parameters.contains("tools") || parameters.contains("toolChoice")) {
            needed[key]["tools"] = Json::object();
            supported = supported && capabilities[key].contains("tools");
        }
        if (parameters.value("includeContext", Json("none")) != "none") {
            needed[key]["context"] = Json::object();
            supported = supported && capabilities[key].contains("context");
        }
    }
    if (!supported)
        throw RpcError(ErrorCode::missing_capability, "Required client capability is missing",
                       {{"requiredCapabilities", std::move(needed)}});
}
void validate_input_response(const Json &request, const Json &response) {
    const auto method = text(request.at("method"));
    schema(type(method, true)).validate(response);
    if (method == "elicitation/create" && request["params"].value("mode", Json("form")) == "form" &&
        response["action"] == "accept") {
        if (!response.contains("content"))
            throw RpcError(ErrorCode::invalid_params, "Accepted form requires content");
        Schema(request["params"]["requestedSchema"]).validate(response["content"]);
    }
}
Json InputHandlers::capabilities() const {
    Json result = Json::object();
    if (elicitation) {
        result["elicitation"] = Json::object();
        if (elicitation_form_enabled)
            result["elicitation"]["form"] = Json::object();
        if (elicitation_url_enabled)
            result["elicitation"]["url"] = Json::object();
    }
    if (sampling) {
        result["sampling"] = Json::object();
        if (sampling_tools)
            result["sampling"]["tools"] = Json::object();
        if (sampling_context)
            result["sampling"]["context"] = Json::object();
    }
    if (roots)
        result["roots"] = Json::object();
    return result;
}
Json InputHandlers::operator()(const Json &request) const {
    validate_input_request(request, capabilities());
    const auto method = text(request["method"]);
    const auto &handler = method == "elicitation/create" ? elicitation
                          : method == "roots/list"       ? roots
                                                         : sampling;
    auto response = handler(request["params"]);
    validate_input_response(request, response);
    return response;
}
} // namespace mcp
