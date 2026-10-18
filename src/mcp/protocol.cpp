#include "request_id.h"
#include <cmath>
#include <mcp/prompt.h>
#include <mcp/protocol.h>
#include <mcp/resource.h>
#include <mcp/tool.h>

namespace mcp {
RpcError::RpcError(ErrorCode code, std::string message, Json data)
    : RpcError(static_cast<int>(code), std::move(message), std::move(data)) {}
RpcError::RpcError(int code, std::string message, Json data)
    : std::runtime_error(std::move(message)), code_(code), data_(std::move(data)) {}
bool valid_id(const Json &id) noexcept {
    if (id.is_string() || id.is_number_integer())
        return true;
    if (!id.is_number_float())
        return false;
    const double value = id.get<double>();
    return std::isfinite(value) && std::trunc(value) == value && value >= -9223372036854775808.0 &&
           value < 18446744073709551616.0;
}
Json make_request(Json id, std::string_view method, Json params, Json capabilities, Json info) {
    if (!valid_id(id) || method.empty() || !params.is_object() || !capabilities.is_object())
        throw RpcError(ErrorCode::invalid_params, "Invalid request construction");
    if (!params.contains("_meta"))
        params["_meta"] = Json::object();
    auto &meta = params["_meta"];
    if (!meta.is_object())
        throw RpcError(ErrorCode::invalid_params, "_meta must be an object");
    meta[version_key] = protocol_version;
    meta[capabilities_key] = std::move(capabilities);
    if (!info.is_null())
        meta[client_info_key] = std::move(info);
    return {{"jsonrpc", "2.0"},
            {"id", std::move(id)},
            {"method", method},
            {"params", std::move(params)}};
}
Json make_notification(std::string_view method, Json params) {
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}
Json make_error(const Json &id, int code, std::string_view message, Json data) {
    Json error = {{"code", code}, {"message", message}};
    if (!data.is_null())
        error["data"] = std::move(data);
    Json response = {{"jsonrpc", "2.0"}, {"error", std::move(error)}};
    if (valid_id(id))
        response["id"] = id;
    return response;
}
void validate_request(const Json &value) {
    if (!value.is_object() || value.value("jsonrpc", Json()) != "2.0" || !value.contains("id") ||
        !valid_id(value["id"]) || !value.contains("method") || !value["method"].is_string() ||
        text(value["method"]).empty() || value.contains("result") || value.contains("error"))
        throw RpcError(ErrorCode::invalid_request, "Invalid JSON-RPC request");
    if (!value.contains("params") || !value["params"].is_object() ||
        !value["params"].contains("_meta") || !value["params"]["_meta"].is_object())
        throw RpcError(ErrorCode::invalid_params,
                       "Per-request _meta is required; supported protocol: 2026-07-28");
    const auto &meta = value["params"]["_meta"];
    if (!meta.contains(version_key) || !meta[version_key].is_string() ||
        !meta.contains(capabilities_key) || !meta[capabilities_key].is_object())
        throw RpcError(ErrorCode::invalid_params,
                       "Protocol version and client capabilities are required");
    if (text(meta[version_key]) != protocol_version)
        throw RpcError(
            ErrorCode::unsupported_version, "Unsupported protocol version",
            {{"supported", Json::array({protocol_version})}, {"requested", meta[version_key]}});
    if (meta.contains("progressToken") && !valid_id(meta["progressToken"]))
        throw RpcError(ErrorCode::invalid_params, "Invalid progress token");
    if (meta.contains("io.modelcontextprotocol/logLevel")) {
        const auto &level = meta["io.modelcontextprotocol/logLevel"];
        if (!level.is_string() || (level != "debug" && level != "info" && level != "notice" &&
                                   level != "warning" && level != "error" && level != "critical" &&
                                   level != "alert" && level != "emergency"))
            throw RpcError(ErrorCode::invalid_params, "Invalid log level");
    }
    if (meta.contains(client_info_key)) {
        const auto &info = meta[client_info_key];
        if (!info.is_object() || !info.contains("name") || !info["name"].is_string() ||
            !info.contains("version") || !info["version"].is_string())
            throw RpcError(ErrorCode::invalid_params, "Invalid client identity");
    }
}
void validate_response(const Json &value, const Json &id) {
    auto fail = [] {
        throw TransportError(TransportErrorKind::invalid_response, "Invalid JSON-RPC response");
    };
    if (!value.is_object() || value.value("jsonrpc", Json()) != "2.0" || !value.contains("id") ||
        !valid_id(value["id"]) || !valid_id(id) ||
        detail::canonical_id(value["id"]) != detail::canonical_id(id) || value.contains("method") ||
        value.contains("result") == value.contains("error"))
        fail();
    if (value.contains("error")) {
        const auto &error = value["error"];
        if (!error.is_object() || !error.contains("code") || !error["code"].is_number_integer() ||
            !error.contains("message") || !error["message"].is_string())
            fail();
    } else {
        const auto &result = value["result"];
        if (!result.is_object())
            fail();
        if (result.contains("resultType") && !result["resultType"].is_string())
            fail();
        if (result.value("resultType", Json()) == "input_required" &&
            !result.contains("inputRequests") && !result.contains("requestState"))
            fail();
        if (result.contains("_meta") && !result["_meta"].is_object())
            fail();
        if (result.contains("requestState") && !result["requestState"].is_string())
            fail();
        if (result.contains("inputRequests")) {
            if (!result["inputRequests"].is_object())
                fail();
            for (const auto &request : result["inputRequests"]) {
                if (!request.is_object() || !request.contains("method") ||
                    !request["method"].is_string() || !request.contains("params") ||
                    !request["params"].is_object())
                    fail();
            }
        }
    }
}
Json text_result(std::string_view value, bool is_error) {
    Json result = {{"resultType", "complete"},
                   {"content", Json::array({{{"type", "text"}, {"text", value}}})}};
    if (is_error)
        result["isError"] = true;
    return result;
}
Json input_required(Json requests, std::string state) {
    if (!requests.is_object())
        throw std::invalid_argument("inputRequests must be an object");
    if (requests.empty() && state.empty())
        throw std::invalid_argument("Input or state is required");
    Json result = {{"resultType", "input_required"}};
    if (!requests.empty())
        result["inputRequests"] = std::move(requests);
    if (!state.empty())
        result["requestState"] = std::move(state);
    return result;
}
Json Tool::descriptor() const {
    Json value = metadata;
    value["name"] = name;
    value["description"] = description;
    value["inputSchema"] = input_schema;
    if (output_schema)
        value["outputSchema"] = *output_schema;
    if (!annotations.empty())
        value["annotations"] = annotations;
    return value;
}
Json Resource::descriptor() const {
    Json value = metadata;
    value["uri"] = uri;
    value["name"] = name;
    if (!description.empty())
        value["description"] = description;
    if (!mime_type.empty())
        value["mimeType"] = mime_type;
    return value;
}
Json ResourceTemplate::descriptor() const {
    Json value = metadata;
    value["uriTemplate"] = uri_template;
    value["name"] = name;
    if (!description.empty())
        value["description"] = description;
    if (!mime_type.empty())
        value["mimeType"] = mime_type;
    return value;
}
Json Prompt::descriptor() const {
    Json value = metadata;
    value["name"] = name;
    value["description"] = description;
    value["arguments"] = arguments;
    return value;
}
Json text_resource(std::string_view uri, std::string_view value, std::string_view mime) {
    return {{"resultType", "complete"},
            {"contents", Json::array({{{"uri", uri}, {"text", value}, {"mimeType", mime}}})}};
}
Json prompt_result(std::string_view value, std::string_view role) {
    return {{"resultType", "complete"},
            {"messages",
             Json::array({{{"role", role}, {"content", {{"type", "text"}, {"text", value}}}}})}};
}
} // namespace mcp
