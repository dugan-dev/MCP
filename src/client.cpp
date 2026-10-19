#include <atomic>
#include <map>
#include <mcp/client.h>
#include <mcp/memory.h>
#include <mcp/schema.h>
#include <mutex>
#include <optional>
#include <set>
#include "deadline.h"
#include "http_headers.h"

namespace mcp {
namespace {
std::atomic<std::uint64_t> next_request_id{1};
void check_options(const RequestOptions& options) {
  if (options.timeout <= std::chrono::milliseconds::zero() || !options.meta.is_object())
    throw std::invalid_argument("Request timeout must be positive and metadata must be an object");
  if (options.stop.stop_requested())
    throw TransportError(TransportErrorKind::cancelled, "Request cancelled");
}
RequestOptions remaining(RequestOptions options, std::chrono::steady_clock::time_point deadline) {
  options.timeout = detail::remaining_time(deadline);
  if (options.timeout <= std::chrono::milliseconds::zero())
    throw TransportError(TransportErrorKind::timeout, "Request deadline exceeded");
  return options;
}
}  // namespace
struct Client::Impl {
  std::shared_ptr<Transport> transport_;
  ClientOptions options_;
  std::mutex schemas_mutex_;
  std::map<std::string, std::optional<Schema>, std::less<>> tool_outputs_;
  Impl(std::shared_ptr<Transport> transport, ClientOptions options) : transport_(std::move(transport)), options_(std::move(options)) {}
};
Client::Client(std::shared_ptr<Transport> transport, ClientOptions options) {
  if (!transport || !options.capabilities.is_object() || !options.max_pages || !options.max_input_rounds || !options.max_list_items ||
      !options.max_list_bytes || !options.max_input_requests)
    throw std::invalid_argument("Invalid client options");
  if (!options.memory_resource)
    options.memory_resource = memory::default_resource();
  JsonScope scope(options.memory_resource);
  impl_ = std::make_shared<Impl>(std::move(transport), std::move(options));
}
Client::~Client() = default;
Json Client::request(std::string_view method, Json params, RequestOptions options, NotificationHandler notify) {
  check_options(options);
  JsonScope scope(impl_->options_.memory_resource);
  if (!params.is_object())
    throw std::invalid_argument("Request params must be an object");
  if (!params.contains("_meta"))
    params["_meta"] = Json::object();
  if (!params["_meta"].is_object())
    throw std::invalid_argument("Request _meta must be an object");
  params["_meta"].update(options.meta);
  const auto id = next_request_id.fetch_add(1, std::memory_order_relaxed);
  if (!id || id > 9007199254740991ULL)
    throw std::overflow_error("Request ID space exhausted");
  Json message = make_request(id, method, std::move(params), impl_->options_.capabilities, impl_->options_.info);
  Json response = impl_->transport_->exchange(message, options, std::move(notify));
  validate_response(response, message["id"]);
  if (response.contains("error")) {
    const auto& error = response["error"];
    int code;
    try {
      code = error["code"].get<int>();
    } catch (...) {
      throw TransportError(TransportErrorKind::invalid_response, "Error code is out of range");
    }
    throw RpcError(code, std::string(text(error["message"])), error.value("data", Json()));
  }
  Json result = std::move(response["result"]);
  if (!result.contains("resultType"))
    result["resultType"] = "complete";
  if (result["resultType"] != "complete" && result["resultType"] != "input_required") {
    if (!impl_->options_.capabilities.contains("extensions") || !impl_->options_.capabilities["extensions"].is_object())
      throw TransportError(TransportErrorKind::invalid_response, "Unnegotiated extension result");
    if (result["resultType"] == "task") {
      const auto& extensions = impl_->options_.capabilities["extensions"];
      if (method != "tools/call" || !extensions.contains("io.modelcontextprotocol/tasks") || !result.contains("taskId") ||
          !result["taskId"].is_string())
        throw TransportError(TransportErrorKind::invalid_response, "Invalid Tasks creation result");
    }
  }
  return result;
}
Json Client::discover(RequestOptions options) {
  JsonScope scope(impl_->options_.memory_resource);
  check_options(options);
  const auto deadline = detail::request_deadline(options.timeout);
  Json result;
  try {
    result = request("server/discover", Json::object(), remaining(options, deadline));
  } catch (const RpcError& error) {
    bool supported = false;
    if (error.code() == static_cast<int>(ErrorCode::unsupported_version) && error.data().is_object() && error.data().contains("supported") &&
        error.data()["supported"].is_array()) {
      for (const auto& version : error.data()["supported"])
        supported |= version.is_string() && text(version) == protocol_version;
    }
    if (!supported)
      throw;
    result = request("server/discover", Json::object(), remaining(options, deadline));
  }
  if (!result.contains("supportedVersions") || !result["supportedVersions"].is_array() || !result.contains("capabilities") ||
      !result["capabilities"].is_object())
    throw TransportError(TransportErrorKind::invalid_response, "Invalid discovery result");
  bool supported = false;
  for (const auto& version : result["supportedVersions"]) {
    if (!version.is_string())
      throw TransportError(TransportErrorKind::invalid_response, "Invalid supported version");
    supported |= text(version) == protocol_version;
  }
  if (!supported)
    throw TransportError(TransportErrorKind::invalid_response, "No supported protocol version");
  return result;
}
Json Client::list(std::string_view method, std::string_view field, RequestOptions options) {
  auto result = list_result(method, field, std::move(options));
  return std::move(result[field]);
}
Json Client::list_result(std::string_view method, std::string_view field, RequestOptions options,
                         std::function<void(const Json&, const Json&)> validate_page) {
  check_options(options);
  JsonScope scope(impl_->options_.memory_resource);
  const auto deadline = detail::request_deadline(options.timeout);
  Json aggregate, params = Json::object();
  std::set<std::string> cursors;
  std::size_t bytes = 0;
  for (std::size_t page = 0; page < impl_->options_.max_pages; ++page) {
    Json result = request(method, params, remaining(options, deadline));
    check_options(options);
    (void)remaining(options, deadline);
    if (result["resultType"] != "complete" || !result.contains(field) || !result[field].is_array())
      throw TransportError(TransportErrorKind::invalid_response, "Invalid list response");
    if (validate_page)
      validate_page(result, aggregate);
    std::optional<Json> cursor;
    if (result.contains("nextCursor"))
      cursor = std::move(result["nextCursor"]);
    auto items = std::move(result[field]);
    result.erase(field);
    result.erase("nextCursor");
    if (aggregate.is_null()) {
      aggregate = std::move(result);
      aggregate[field] = Json::array();
    }
    for (auto& item : items) {
      const auto size = serialize_json(item).size();
      if (aggregate[field].size() >= impl_->options_.max_list_items || size > impl_->options_.max_list_bytes - bytes)
        throw TransportError(TransportErrorKind::limit, "List aggregation exceeds limit");
      bytes += size;
      aggregate[field].push_back(std::move(item));
    }
    (void)remaining(options, deadline);
    if (!cursor)
      return aggregate;
    if (!cursor->is_string() || text(*cursor).empty() || !cursors.emplace(text(*cursor)).second)
      throw TransportError(TransportErrorKind::invalid_response, "Invalid or repeated pagination cursor");
    params["cursor"] = std::move(*cursor);
  }
  throw TransportError(TransportErrorKind::limit, "Pagination exceeds page limit");
}
Json Client::list_tools(RequestOptions options) {
  JsonScope scope(impl_->options_.memory_resource);
  auto tools = list("tools/list", "tools", std::move(options));
  Json accepted = Json::array();
  std::map<std::string, std::optional<Schema>, std::less<>> outputs;
  for (const auto& tool : tools) {
    if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string() || !tool.contains("inputSchema") || !tool["inputSchema"].is_object())
      throw TransportError(TransportErrorKind::invalid_response, "Invalid tool descriptor");
    try {
      detail::validate_header_schema(tool["inputSchema"]);
    } catch (const std::invalid_argument&) {
      // A malformed header annotation invalidates this tool, not the
      // healthy descriptors in the same listing (2026-07-28).
      continue;
    }
    try {
      std::optional<Schema> output;
      if (tool.contains("outputSchema"))
        output.emplace(tool["outputSchema"]);
      if (!outputs.emplace(std::string(text(tool["name"])), std::move(output)).second)
        throw TransportError(TransportErrorKind::invalid_response, "Duplicate tool name");
    } catch (const std::invalid_argument&) {
      throw TransportError(TransportErrorKind::invalid_response, "Invalid tool output schema");
    }
    accepted.push_back(tool);
  }
  tools = std::move(accepted);
  try {
    impl_->transport_->set_tool_schemas(tools);
  } catch (const std::exception&) {
    throw TransportError(TransportErrorKind::invalid_response, "Invalid tool input schema");
  }
  std::lock_guard lock(impl_->schemas_mutex_);
  impl_->tool_outputs_ = std::move(outputs);
  return tools;
}
Json Client::call_tool(std::string_view name, Json arguments, RequestOptions options, NotificationHandler notify) {
  JsonScope scope(impl_->options_.memory_resource);
  check_options(options);
  const auto deadline = detail::request_deadline(options.timeout);
  bool known;
  {
    std::lock_guard lock(impl_->schemas_mutex_);
    known = impl_->tool_outputs_.contains(name);
  }
  if (!known)
    (void)list_tools(remaining(options, deadline));
  std::optional<Schema> output;
  {
    std::lock_guard lock(impl_->schemas_mutex_);
    if (const auto it = impl_->tool_outputs_.find(name); it != impl_->tool_outputs_.end())
      output = it->second;
  }
  auto result = request("tools/call", {{"name", name}, {"arguments", std::move(arguments)}}, remaining(options, deadline), std::move(notify));
  if (result["resultType"] == "complete") {
    if (!result.contains("content") || !result["content"].is_array() || (result.contains("isError") && !result["isError"].is_boolean()))
      throw TransportError(TransportErrorKind::invalid_response, "Invalid tool result");
    if (output && !result.value("isError", false) && (!result.contains("structuredContent") || !output->accepts(result["structuredContent"])))
      throw TransportError(TransportErrorKind::invalid_response, "Tool output does not satisfy schema");
  }
  return result;
}
Json Client::list_resources(RequestOptions options) {
  return list("resources/list", "resources", std::move(options));
}
Json Client::list_resource_templates(RequestOptions options) {
  return list("resources/templates/list", "resourceTemplates", std::move(options));
}
Json Client::read_resource(std::string_view uri, RequestOptions options) {
  JsonScope scope(impl_->options_.memory_resource);
  return request("resources/read", {{"uri", uri}}, std::move(options));
}
Json Client::list_prompts(RequestOptions options) {
  return list("prompts/list", "prompts", std::move(options));
}
Json Client::get_prompt(std::string_view name, Json arguments, RequestOptions options) {
  JsonScope scope(impl_->options_.memory_resource);
  return request("prompts/get", {{"name", name}, {"arguments", std::move(arguments)}}, std::move(options));
}
Json Client::complete(Json reference, Json argument, RequestOptions options) {
  JsonScope scope(impl_->options_.memory_resource);
  return request("completion/complete", {{"ref", std::move(reference)}, {"argument", std::move(argument)}}, std::move(options));
}
Json Client::listen(Json filters, RequestOptions options, NotificationHandler notify) {
  JsonScope scope(impl_->options_.memory_resource);
  return request("subscriptions/listen", {{"notifications", std::move(filters)}}, std::move(options), std::move(notify));
}
Json Client::request_with_input(std::string_view method, Json params, InputHandler handler, RequestOptions options, NotificationHandler notify) {
  JsonScope scope(impl_->options_.memory_resource);
  check_options(options);
  const auto deadline = detail::request_deadline(options.timeout);
  for (std::size_t round = 0; round < impl_->options_.max_input_rounds; ++round) {
    Json result = request(method, params, remaining(options, deadline), notify);
    if (result["resultType"] == "complete" || result["resultType"] == "task")
      return result;
    params.erase("inputResponses");
    params.erase("requestState");
    if (result.contains("requestState"))
      params["requestState"] = result["requestState"];
    if (result.contains("inputRequests")) {
      if (!result["inputRequests"].is_object() || result["inputRequests"].size() > impl_->options_.max_input_requests)
        throw TransportError(TransportErrorKind::invalid_response, "Invalid inputRequests");
      if (!result["inputRequests"].empty() && !handler)
        throw TransportError(TransportErrorKind::invalid_response, "Input handler is required");
      Json responses = Json::object();
      for (auto it = result["inputRequests"].begin(); it != result["inputRequests"].end(); ++it)
        responses[it.key()] = handler(it.value());
      params["inputResponses"] = std::move(responses);
    }
  }
  throw TransportError(TransportErrorKind::limit, "MRTR exceeds round limit");
}
void Client::close() noexcept {
  impl_->transport_->close();
}
}  // namespace mcp
