#include "http_headers.h"
#include <charconv>
#include <cmath>
#include <map>
#include <mcp/schema.h>
#include <mcp/features.h>
#include <mcp/extensions.h>
#include <mcp/server.h>
#include <mcp/memory.h>
#include <set>
#include <shared_mutex>

namespace mcp {
namespace {
void need(bool condition, std::string_view message) {
    if (!condition)
        throw RpcError(ErrorCode::invalid_params, std::string(message));
}
void require_string(const Json &value, std::string_view key) {
    need(value.contains(key) && value[key].is_string(), std::string(key) + " must be a string");
}
void validate_info(const Json &info) {
    if (!info.is_object() || !info.contains("name") || !info["name"].is_string() ||
        !info.contains("version") || !info["version"].is_string())
        throw std::invalid_argument("Implementation needs name and version");
}
void validate_content(const Json &value) {
    if (!value.is_object() || !value.contains("type") || !value["type"].is_string())
        throw std::runtime_error("Invalid content block");
    const auto type = text(value["type"]);
    auto string = [&](std::string_view key) {
        return value.contains(key) && value[key].is_string();
    };
    bool valid = false;
    if (type == "text")
        valid = string("text");
    else if (type == "image" || type == "audio")
        valid = string("data") && string("mimeType");
    else if (type == "resource_link")
        valid = string("uri") && string("name");
    else if (type == "resource") {
        valid = value.contains("resource") && value["resource"].is_object();
        if (valid) {
            const auto &r = value["resource"];
            valid = r.contains("uri") && r["uri"].is_string() &&
                    ((r.contains("text") && r["text"].is_string()) ||
                     (r.contains("blob") && r["blob"].is_string()));
        }
    }
    if (!valid)
        throw std::runtime_error("Invalid content block");
}
void validate_input_required(const Json &result, const Json &capabilities) {
    if (!result.contains("inputRequests") && !result.contains("requestState"))
        throw std::runtime_error("InputRequiredResult needs inputRequests or requestState");
    if (result.contains("requestState") && !result["requestState"].is_string())
        throw std::runtime_error("requestState must be a string");
    if (!result.contains("inputRequests"))
        return;
    if (!result["inputRequests"].is_object())
        throw std::runtime_error("inputRequests must be an object");
    for (const auto &request : result["inputRequests"]) {
        validate_input_request(request, capabilities);
        if (!request.is_object() || !request.contains("method") || !request["method"].is_string() ||
            !request.contains("params") || !request["params"].is_object())
            throw std::runtime_error("Invalid input request");
        std::string capability;
        const auto method = text(request["method"]);
        if (method == "elicitation/create")
            capability = "elicitation";
        else if (method == "roots/list")
            capability = "roots";
        else if (method == "sampling/createMessage")
            capability = "sampling";
        else
            throw std::runtime_error("Unknown input request method");
        Json required = {{capability, Json::object()}};
        bool supported = capabilities.contains(capability) && capabilities[capability].is_object();
        if (capability == "elicitation") {
            const auto mode = request["params"].value("mode", Json("form"));
            if (mode != "form" && mode != "url")
                throw std::runtime_error("Invalid elicitation mode");
            required[capability][text(mode)] = Json::object();
            if (supported) {
                const auto &declared = capabilities[capability];
                supported = mode == "form" ? (declared.empty() || declared.contains("form"))
                                           : declared.contains("url");
            }
            if (!request["params"].contains("message") || !request["params"]["message"].is_string())
                throw std::runtime_error("Elicitation message is required");
            if (mode == "form" && !request["params"].contains("requestedSchema"))
                throw std::runtime_error("Elicitation schema is required");
            if (mode == "url" &&
                (!request["params"].contains("url") || !request["params"]["url"].is_string()))
                throw std::runtime_error("Elicitation URL is required");
        }
        if (!supported)
            throw RpcError(ErrorCode::missing_capability, "Required client capability is missing",
                           {{"requiredCapabilities", std::move(required)}});
    }
}
struct Entry {
    Json descriptor;
    Handler handler;
    std::optional<Schema> input;
    std::optional<Schema> output;
};
using Registry = std::pmr::map<std::pmr::string, std::shared_ptr<const Entry>, std::less<>>;
struct Sub {
    std::recursive_mutex mutex;
    bool active = true;
    Json id, filter;
    Json attributes;
    std::map<std::string, std::string, std::less<>> task_owners;
    NotificationHandler send;
};
} // namespace
struct Server::Impl {
    ServerOptions options;
    // Registry containers are destroyed before their caller-owned context.
    // Escaping entries, subscriptions and weak Impl blocks use std::shared_ptr.
    unimem::Global memory;
    mutable std::shared_mutex mutex;
    Registry tools, resources, templates, prompts, extensions;
    std::uint64_t revision = 1;
    Handler completion, resource_resolver;
    Json extension_caps = Json::object();
    TaskSubscriptionAuthorizer task_authorizer;
    std::pmr::map<std::uint64_t, std::shared_ptr<Sub>> subscriptions;
    std::uint64_t next_subscription = 1;
    explicit Impl(ServerOptions value)
        : options(std::move(value)),
          tools(memory.resource()), resources(memory.resource()), templates(memory.resource()),
          prompts(memory.resource()), extensions(memory.resource()),
          subscriptions(memory.resource()) {}
    Json stamp(Json result, bool cacheable = false, bool task_allowed = false) const {
        if (!result.is_object())
            throw std::runtime_error("Handler result must be an object");
        if (!result.contains("resultType"))
            result["resultType"] = "complete";
        if (result["resultType"] != "complete" && result["resultType"] != "input_required" &&
            !(task_allowed && result["resultType"] == "task"))
            throw std::runtime_error("Unsupported resultType");
        if (!result.contains("_meta"))
            result["_meta"] = Json::object();
        if (!result["_meta"].is_object())
            throw std::runtime_error("Result _meta must be an object");
        result["_meta"][server_info_key] = options.info;
        if (cacheable && result["resultType"] == "complete") {
            result["ttlMs"] = options.cache_ttl_ms;
            result["cacheScope"] = options.cache_scope;
        }
        return result;
    }
    Json capability_snapshot() const {
        Json result = {{"tools", {{"listChanged", true}}},
                       {"prompts", {{"listChanged", true}}},
                       {"resources", {{"listChanged", true}, {"subscribe", true}}}};
        if (completion)
            result["completions"] = Json::object();
        if (!extension_caps.empty())
            result["extensions"] = extension_caps;
        return result;
    }
    Json list(const Registry &registry, std::string_view method, std::string_view field,
              const Json &params) const {
        std::size_t offset = 0;
        if (params.contains("cursor")) {
            need(params["cursor"].is_string(), "Cursor must be a string");
            std::string_view cursor = text(params["cursor"]);
            const std::string prefix = std::string(method) + ";";
            need(cursor.starts_with(prefix), "Cursor belongs to another list");
            cursor.remove_prefix(prefix.size());
            const auto separator = cursor.find(';');
            need(separator != std::string_view::npos, "Invalid cursor");
            std::uint64_t rev = 0;
            const auto [p, e] = std::from_chars(cursor.data(), cursor.data() + separator, rev);
            need(e == std::errc() && p == cursor.data() + separator && rev == revision,
                 "Stale or invalid cursor; restart listing");
            cursor.remove_prefix(separator + 1);
            const auto [q, f] =
                std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
            need(f == std::errc() && q == cursor.data() + cursor.size() &&
                     offset <= registry.size(),
                 "Invalid cursor offset");
        }
        Json items = Json::array();
        auto it = registry.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(offset));
        while (it != registry.end() && items.size() < options.limits.page_size) {
            items.push_back(it->second->descriptor);
            ++it;
        }
        Json result = {{field, std::move(items)}};
        if (it != registry.end())
            result["nextCursor"] = std::string(method) + ";" + std::to_string(revision) + ";" +
                                   std::to_string(offset + options.limits.page_size);
        return stamp(std::move(result), true);
    }
    void publish(std::string_view method, Json params = Json::object(),
                 std::string_view task_owner = {}) {
        JsonScope scope(options.memory_resource);
        std::vector<std::shared_ptr<Sub>> snapshot;
        TaskSubscriptionAuthorizer authorize_task;
        {
            std::shared_lock lock(mutex);
            authorize_task = task_authorizer;
            for (const auto &[key, sub] : subscriptions) {
                (void)key;
                snapshot.push_back(sub);
            }
        }
        for (const auto &sub : snapshot) {
            std::lock_guard lock(sub->mutex);
            if (!sub->active)
                continue;
            bool accepted = false;
            if (method == "notifications/tools/list_changed")
                accepted = sub->filter.value("toolsListChanged", false);
            else if (method == "notifications/prompts/list_changed")
                accepted = sub->filter.value("promptsListChanged", false);
            else if (method == "notifications/resources/list_changed")
                accepted = sub->filter.value("resourcesListChanged", false);
            else if (method == "notifications/resources/updated" &&
                     sub->filter.contains("resourceSubscriptions")) {
                for (const auto &uri : sub->filter["resourceSubscriptions"])
                    if (uri == params["uri"])
                        accepted = true;
            }
            else if (method == "notifications/tasks" && sub->filter.contains("taskIds") &&
                     params.contains("taskId")) {
                const auto id = std::string(text(params["taskId"]));
                const auto owner = sub->task_owners.find(id);
                if (owner != sub->task_owners.end() && owner->second == task_owner &&
                    authorize_task) {
                    try {
                        accepted = authorize_task(sub->attributes, id) == task_owner;
                    } catch (...) {
                        // Expired, erased or revoked access never delivers task data.
                        accepted = false;
                    }
                }
            }
            if (accepted) {
                Json message_params = params;
                message_params["_meta"][subscription_key] = sub->id;
                try {
                    sub->send(make_notification(method, std::move(message_params)));
                } catch (...) {
                    sub->active = false;
                }
            }
        }
    }
};
const Json &RequestContext::id() const noexcept {
    return (*request_)["id"];
}
const Json &RequestContext::params() const noexcept {
    return (*request_)["params"];
}
const Json &RequestContext::capabilities() const noexcept {
    return params()["_meta"][capabilities_key];
}
const Json &RequestContext::attributes() const noexcept {
    return *attributes_;
}
std::stop_token RequestContext::stop_token() const noexcept {
    return stop_;
}
void RequestContext::throw_if_cancelled() const {
    if (stop_.stop_requested())
        throw TransportError(TransportErrorKind::cancelled, "Request cancelled");
}
void RequestContext::progress(double value, std::optional<double> total, std::string_view message) {
    throw_if_cancelled();
    if (!std::isfinite(value) || value < 0 || (last_progress_ && value <= *last_progress_) ||
        (total && (!std::isfinite(*total) || *total < value)))
        throw std::invalid_argument("Invalid progress sequence");
    last_progress_ = value;
    if (!notify_ || !params()["_meta"].contains("progressToken"))
        return;
    Json p = {{"progressToken", params()["_meta"]["progressToken"]}, {"progress", value}};
    if (total)
        p["total"] = *total;
    if (!message.empty())
        p["message"] = message;
    notify_(make_notification("notifications/progress", std::move(p)));
}
void RequestContext::require_capability(std::string_view name) const {
    if (!capabilities().contains(name))
        throw RpcError(ErrorCode::missing_capability, "Required client capability is missing",
                       {{"requiredCapabilities", {{name, Json::object()}}}});
}
Json RequestContext::elicit(std::string_view key, Json parameters, std::string state) const {
    Json requests = {{key, {{"method", "elicitation/create"}, {"params", std::move(parameters)}}}};
    Json result = input_required(std::move(requests), std::move(state));
    validate_input_required(result, capabilities());
    return result;
}
Json RequestContext::request_input(std::string_view key, Json request, std::string state) const {
    Json result = input_required({{key, std::move(request)}}, std::move(state));
    validate_input_required(result, capabilities());
    return result;
}
void RequestContext::log(std::string_view level, Json data, std::string_view logger) const {
    const auto actual = parse_log_level(level);
    const auto &meta = params()["_meta"];
    if (!notify_ || !meta.contains("io.modelcontextprotocol/logLevel")) return;
    const auto threshold = parse_log_level(text(meta["io.modelcontextprotocol/logLevel"]));
    if (static_cast<int>(actual) < static_cast<int>(threshold)) return;
    Json message = {{"level", level}, {"data", std::move(data)}};
    if (!logger.empty()) message["logger"] = logger;
    notify_(make_notification("notifications/message", std::move(message)));
}
Server::Server(ServerOptions options) {
    validate_info(options.info);
    if (!options.limits.page_size || !options.limits.message_bytes || !options.limits.json_depth ||
        !options.limits.pending_requests || !options.limits.stream_queue_bytes ||
        !options.max_subscriptions ||
        (options.cache_scope != "public" && options.cache_scope != "private"))
        throw std::invalid_argument("Invalid server limits or cache scope");
    if (!options.memory_resource)
        options.memory_resource = memory::default_resource();
    JsonScope scope(options.memory_resource);
    impl_ = std::make_shared<Impl>(std::move(options));
}
Server::~Server() = default;
const ServerOptions &Server::options() const noexcept {
    return impl_->options;
}
void Server::add_tool(Tool tool, Handler handler) {
    if (tool.name.empty() || !handler || !tool.metadata.is_object() ||
        !tool.annotations.is_object() || !tool.input_schema.is_object() ||
        tool.input_schema.value("type", Json()) != "object")
        throw std::invalid_argument("Tool needs name, object input schema and handler");
    JsonScope scope(impl_->options.memory_resource);
    auto entry = std::make_shared<Entry>();
    entry->input.emplace(tool.input_schema);
    detail::validate_header_schema(tool.input_schema);
    if (tool.output_schema)
        entry->output.emplace(*tool.output_schema);
    entry->descriptor = tool.descriptor();
    entry->handler = std::move(handler);
    {
        std::unique_lock lock(impl_->mutex);
        impl_->tools.insert_or_assign(std::pmr::string(tool.name, impl_->memory.resource()), entry);
        ++impl_->revision;
    }
    impl_->publish("notifications/tools/list_changed");
}
bool Server::remove_tool(std::string_view name) {
    JsonScope scope(impl_->options.memory_resource);
    bool erased;
    {
        std::unique_lock lock(impl_->mutex);
        auto it = impl_->tools.find(name);
        erased = it != impl_->tools.end();
        if (erased) {
            impl_->tools.erase(it);
            ++impl_->revision;
        }
    }
    if (erased)
        impl_->publish("notifications/tools/list_changed");
    return erased;
}
void Server::add_resource(Resource resource, Handler handler) {
    if (resource.uri.empty() || resource.name.empty() || !handler || !resource.metadata.is_object())
        throw std::invalid_argument("Invalid resource");
    JsonScope scope(impl_->options.memory_resource);
    auto entry = std::make_shared<Entry>();
    entry->descriptor = resource.descriptor();
    entry->handler = std::move(handler);
    {
        std::unique_lock lock(impl_->mutex);
        impl_->resources.insert_or_assign(std::pmr::string(resource.uri, impl_->memory.resource()),
                                          entry);
        ++impl_->revision;
    }
    impl_->publish("notifications/resources/list_changed");
}
bool Server::remove_resource(std::string_view uri) {
    JsonScope scope(impl_->options.memory_resource);
    bool erased;
    {
        std::unique_lock lock(impl_->mutex);
        auto it = impl_->resources.find(uri);
        erased = it != impl_->resources.end();
        if (erased) {
            impl_->resources.erase(it);
            ++impl_->revision;
        }
    }
    if (erased)
        impl_->publish("notifications/resources/list_changed");
    return erased;
}
void Server::add_resource_template(ResourceTemplate resource) {
    if (resource.uri_template.empty() || resource.name.empty() || !resource.metadata.is_object())
        throw std::invalid_argument("Invalid resource template");
    JsonScope scope(impl_->options.memory_resource);
    auto entry = std::make_shared<Entry>();
    entry->descriptor = resource.descriptor();
    {
        std::unique_lock lock(impl_->mutex);
        impl_->templates.insert_or_assign(
            std::pmr::string(resource.uri_template, impl_->memory.resource()), entry);
        ++impl_->revision;
    }
    impl_->publish("notifications/resources/list_changed");
}
void Server::set_resource_resolver(Handler handler) {
    std::unique_lock lock(impl_->mutex);
    impl_->resource_resolver = std::move(handler);
}
void Server::add_prompt(Prompt prompt, Handler handler) {
    if (prompt.name.empty() || !handler || !prompt.arguments.is_array() ||
        !prompt.metadata.is_object())
        throw std::invalid_argument("Invalid prompt");
    std::set<std::string> names;
    for (const auto &argument : prompt.arguments) {
        if (!argument.is_object() || !argument.contains("name") || !argument["name"].is_string() ||
            (argument.contains("required") && !argument["required"].is_boolean()) ||
            !names.emplace(text(argument["name"])).second)
            throw std::invalid_argument("Invalid or duplicate prompt argument");
    }
    JsonScope scope(impl_->options.memory_resource);
    auto entry = std::make_shared<Entry>();
    entry->descriptor = prompt.descriptor();
    entry->handler = std::move(handler);
    {
        std::unique_lock lock(impl_->mutex);
        impl_->prompts.insert_or_assign(std::pmr::string(prompt.name, impl_->memory.resource()),
                                        entry);
        ++impl_->revision;
    }
    impl_->publish("notifications/prompts/list_changed");
}
bool Server::remove_prompt(std::string_view name) {
    JsonScope scope(impl_->options.memory_resource);
    bool erased;
    {
        std::unique_lock lock(impl_->mutex);
        auto it = impl_->prompts.find(name);
        erased = it != impl_->prompts.end();
        if (erased) {
            impl_->prompts.erase(it);
            ++impl_->revision;
        }
    }
    if (erased)
        impl_->publish("notifications/prompts/list_changed");
    return erased;
}
void Server::set_completion_handler(Handler handler) {
    std::unique_lock lock(impl_->mutex);
    impl_->completion = std::move(handler);
}
void Server::add_extension(std::string identifier, Json settings, std::string method,
                           Handler handler) {
    if (identifier.find('/') == std::string::npos || method.empty() || !settings.is_object() ||
        !handler || method == "server/discover" || method.starts_with("tools/") ||
        (method.starts_with("resources/") &&
         !(identifier == "io.modelcontextprotocol/skills" && method == "resources/directory/read")) ||
        method.starts_with("prompts/") ||
        method.starts_with("subscriptions/") || method == "completion/complete")
        throw std::invalid_argument("Invalid extension registration");
    JsonScope scope(impl_->options.memory_resource);
    auto entry = std::make_shared<Entry>();
    entry->handler = std::move(handler);
    entry->descriptor = {{"extension", identifier}};
    std::unique_lock lock(impl_->mutex);
    impl_->extension_caps[identifier] = std::move(settings);
    impl_->extensions.insert_or_assign(std::pmr::string(method, impl_->memory.resource()), entry);
    ++impl_->revision;
}
Json Server::capabilities() const {
    JsonScope scope(impl_->options.memory_resource);
    std::shared_lock lock(impl_->mutex);
    return impl_->capability_snapshot();
}
Json Server::tool_schema(std::string_view name) const {
    JsonScope scope(impl_->options.memory_resource);
    std::shared_lock lock(impl_->mutex);
    const auto it = impl_->tools.find(name);
    return it == impl_->tools.end() ? Json() : it->second->descriptor["inputSchema"];
}
Json Server::dispatch(std::string_view bytes, std::stop_token stop,
                      NotificationHandler notify) const {
    JsonScope scope(impl_->options.memory_resource);
    try {
        return dispatch(parse_json(bytes, impl_->options.limits.message_bytes,
                                   impl_->options.limits.json_depth),
                        stop, std::move(notify));
    } catch (const RpcError &error) {
        return make_error(nullptr, error.code(), error.what(), error.data());
    }
}
Json Server::dispatch(const Json &request, std::stop_token stop, NotificationHandler notify) const {
    return dispatch(request, DispatchOptions{stop, std::move(notify), nullptr});
}
Json Server::dispatch(const Json &request, DispatchOptions options) const {
    JsonScope scope(impl_->options.memory_resource);
    Json id;
    if (request.is_object() && request.contains("id") && valid_id(request["id"]))
        id = request["id"];
    if (request.is_object() && !request.contains("id") && request.contains("method"))
        return nullptr;
    try {
        validate_request(request);
        RequestContext context;
        context.request_ = &request;
        context.stop_ = options.stop;
        context.notify_ = std::move(options.notify);
        context.attributes_ = &options.attributes;
        context.throw_if_cancelled();
        const auto method = text(request["method"]);
        const auto &params = request["params"];
        Json result;
        std::shared_ptr<const Entry> entry;
        Handler fallback;
        bool cacheable = false;
        if (method == "server/discover") {
            std::shared_lock lock(impl_->mutex);
            result = {{"supportedVersions", Json::array({protocol_version})},
                      {"capabilities", impl_->capability_snapshot()}};
            if (!impl_->options.instructions.empty())
                result["instructions"] = impl_->options.instructions;
            cacheable = true;
        } else if (method == "tools/list" || method == "resources/list" ||
                   method == "resources/templates/list" || method == "prompts/list") {
            std::shared_lock lock(impl_->mutex);
            if (method == "tools/list")
                result = impl_->list(impl_->tools, method, "tools", params);
            else if (method == "resources/list")
                result = impl_->list(impl_->resources, method, "resources", params);
            else if (method == "prompts/list")
                result = impl_->list(impl_->prompts, method, "prompts", params);
            else
                result = impl_->list(impl_->templates, method, "resourceTemplates", params);
        } else if (method == "tools/call" || method == "prompts/get" ||
                   method == "resources/read") {
            const auto key = method == "resources/read" ? "uri" : "name";
            require_string(params, key);
            {
                std::shared_lock lock(impl_->mutex);
                const auto &registry = method == "tools/call"    ? impl_->tools
                                       : method == "prompts/get" ? impl_->prompts
                                                                 : impl_->resources;
                auto it = registry.find(text(params[key]));
                if (it != registry.end())
                    entry = it->second;
                else if (method == "resources/read")
                    fallback = impl_->resource_resolver;
            }
            need(entry || fallback, "Unknown tool, prompt or resource");
            Json arguments = params.value("arguments", Json::object());
            if (method != "resources/read")
                need(arguments.is_object(), "Arguments must be an object");
            if (method == "tools/call")
                entry->input->validate(arguments);
            if (method == "prompts/get") {
                for (const auto &arg : entry->descriptor["arguments"]) {
                    const auto name = text(arg["name"]);
                    if (arg.value("required", false))
                        need(arguments.contains(name), "Missing prompt argument");
                }
                for (const auto &value : arguments)
                    need(value.is_string(), "Prompt arguments must be strings");
            }
            if (method == "resources/read")
                result = entry ? entry->handler(params, context) : fallback(params, context);
            else {
                try {
                    result = entry->handler(arguments, context);
                } catch (const RpcError &) {
                    throw;
                } catch (const TransportError &) {
                    throw;
                } catch (...) {
                    if (method != "tools/call")
                        throw;
                    result = text_result("Tool execution failed", true);
                }
            }
            if (result.is_object() &&
                result.value("resultType", Json("complete")) == "input_required") {
                validate_input_required(result, context.capabilities());
            } else if (method == "tools/call" && result.value("resultType", Json("complete")) == "task") {
                const auto &caps = context.capabilities();
                need(caps.contains("extensions") && caps["extensions"].is_object() &&
                     caps["extensions"].contains("io.modelcontextprotocol/tasks"),
                     "Task result requires Tasks client capability");
                extensions::validate_task(result, true);
            } else if (method == "tools/call") {
                if (!result.is_object() || !result.contains("content") ||
                    !result["content"].is_array() ||
                    (result.contains("isError") && !result["isError"].is_boolean()))
                    throw std::runtime_error("Invalid tool result");
                for (const auto &content : result["content"])
                    validate_content(content);
                if (entry->output && !result.value("isError", false)) {
                    if (!result.contains("structuredContent") ||
                        !entry->output->accepts(result["structuredContent"]))
                        throw std::runtime_error("Invalid structured tool output");
                }
            } else if (method == "prompts/get") {
                if (!result.contains("messages") || !result["messages"].is_array())
                    throw std::runtime_error("Invalid prompt result");
                for (const auto &message : result["messages"]) {
                    if (!message.is_object() || !message.contains("role") ||
                        (message["role"] != "user" && message["role"] != "assistant") ||
                        !message.contains("content"))
                        throw std::runtime_error("Invalid prompt message");
                    validate_content(message["content"]);
                }
            } else {
                if (!result.contains("contents") || !result["contents"].is_array())
                    throw std::runtime_error("Invalid resource result");
                for (const auto &content : result["contents"]) {
                    if (!content.is_object() || !content.contains("uri") ||
                        !content["uri"].is_string() ||
                        !((content.contains("text") && content["text"].is_string()) ||
                          (content.contains("blob") && content["blob"].is_string())))
                        throw std::runtime_error("Invalid resource contents");
                }
                cacheable = true;
            }
        } else if (method == "completion/complete") {
            need(params.contains("ref") && params["ref"].is_object() &&
                     params["ref"].contains("type"),
                 "Completion reference is required");
            const auto &ref = params["ref"];
            need(ref["type"] == "ref/prompt" || ref["type"] == "ref/resource",
                 "Invalid completion reference");
            require_string(ref, ref["type"] == "ref/prompt" ? "name" : "uri");
            need(params.contains("argument") && params["argument"].is_object(),
                 "Completion argument is required");
            require_string(params["argument"], "name");
            require_string(params["argument"], "value");
            {
                std::shared_lock lock(impl_->mutex);
                fallback = impl_->completion;
            }
            if (!fallback)
                throw RpcError(ErrorCode::method_not_found, "Completion is not supported");
            result = fallback(params, context);
            if (!result.is_object() || !result.contains("completion") ||
                !result["completion"].is_object() || !result["completion"].contains("values") ||
                !result["completion"]["values"].is_array() ||
                result["completion"]["values"].size() > 100)
                throw std::runtime_error("Invalid completion result");
            for (const auto &value : result["completion"]["values"])
                if (!value.is_string())
                    throw std::runtime_error("Invalid completion value");
        } else {
            {
                std::shared_lock lock(impl_->mutex);
                const auto it = impl_->extensions.find(method);
                if (it != impl_->extensions.end())
                    entry = it->second;
            }
            if (!entry)
                throw RpcError(ErrorCode::method_not_found,
                               std::string("Unknown method; supported protocol: ") +
                                   std::string(protocol_version));
            const auto identifier = text(entry->descriptor["extension"]);
            if (!context.capabilities().contains("extensions") ||
                !context.capabilities()["extensions"].is_object() ||
                !context.capabilities()["extensions"].contains(identifier))
                throw RpcError(
                    ErrorCode::missing_capability, "Required extension is missing",
                    {{"requiredCapabilities", {{"extensions", {{identifier, Json::object()}}}}}});
            result = entry->handler(params, context);
        }
        context.throw_if_cancelled();
        if (method != "tools/call" && method != "prompts/get" && method != "resources/read" &&
            result.is_object() && result.value("resultType", Json()) == "input_required")
            throw std::runtime_error("MRTR is unsupported for this method");
        return {
            {"jsonrpc", "2.0"}, {"id", id},
            {"result", impl_->stamp(std::move(result), cacheable,
                method == "tools/call" && context.capabilities().contains("extensions") &&
                context.capabilities()["extensions"].is_object() &&
                context.capabilities()["extensions"].contains("io.modelcontextprotocol/tasks"))}};
    } catch (const RpcError &error) {
        return make_error(id, error.code(), error.what(), error.data());
    } catch (const TransportError &) {
        return make_error(id, 1001, "Request cancelled");
    } catch (...) {
        return make_error(id, static_cast<int>(ErrorCode::internal_error), "Internal error");
    }
}
Subscription::~Subscription() {
    try {
        close();
    } catch (...) {
    }
}
void Subscription::close() {
    std::function<void()> function;
    {
        std::lock_guard lock(mutex_);
        function = std::exchange(close_, {});
    }
    if (function)
        function();
}
void Server::set_task_subscription_authorizer(TaskSubscriptionAuthorizer authorize) {
    std::unique_lock lock(impl_->mutex);
    impl_->task_authorizer = std::move(authorize);
}
std::shared_ptr<Subscription> Server::subscribe(const Json &request, NotificationHandler send,
                                               Json trusted_attributes) {
    JsonScope scope(impl_->options.memory_resource);
    validate_request(request);
    need(request["method"] == "subscriptions/listen", "Expected subscriptions/listen");
    const auto &params = request["params"];
    need(params.contains("notifications") && params["notifications"].is_object(),
         "Subscription filter is required");
    need(static_cast<bool>(send), "Subscription requires a notification channel");
    const auto &filter = params["notifications"];
    Json accepted = Json::object();
    std::map<std::string, std::string, std::less<>> task_owners;
    for (std::string_view key : {"toolsListChanged", "promptsListChanged", "resourcesListChanged"})
        if (filter.contains(key)) {
            need(filter[key].is_boolean(), "Subscription flags must be booleans");
            if (filter[key].get<bool>())
                accepted[key] = true;
        }
    if (filter.contains("resourceSubscriptions")) {
        need(filter["resourceSubscriptions"].is_array(), "Resource subscriptions must be an array");
        need(filter["resourceSubscriptions"].size() <= impl_->options.max_subscriptions,
             "Too many resource subscriptions");
        for (const auto &uri : filter["resourceSubscriptions"])
            need(uri.is_string(), "Resource URI must be a string");
        accepted["resourceSubscriptions"] = filter["resourceSubscriptions"];
    }
    if (filter.contains("taskIds")) {
        need(filter["taskIds"].is_array() && filter["taskIds"].size() <= impl_->options.max_subscriptions,
             "Task IDs must be a bounded array");
        const auto &caps = params["_meta"][capabilities_key];
        if (!caps.contains("extensions") || !caps["extensions"].is_object() ||
            !caps["extensions"].contains("io.modelcontextprotocol/tasks") ||
            !caps["extensions"]["io.modelcontextprotocol/tasks"].is_object())
            throw RpcError(ErrorCode::missing_capability, "Tasks extension capability is required",
                           {{"requiredCapabilities", {{"extensions",
                             {{"io.modelcontextprotocol/tasks", Json::object()}}}}}});
        TaskSubscriptionAuthorizer authorize;
        {
            std::shared_lock lock(impl_->mutex);
            need(impl_->extension_caps.contains("io.modelcontextprotocol/tasks") &&
                     static_cast<bool>(impl_->task_authorizer),
                 "Task subscriptions require an installed access policy");
            authorize = impl_->task_authorizer;
        }
        for (const auto &id : filter["taskIds"]) {
            need(id.is_string() && !text(id).empty(), "Task ID must be a nonempty string");
            auto owner = authorize(trusted_attributes, text(id));
            need(!owner.empty(), "Task access policy returned an empty owner");
            task_owners.emplace(std::string(text(id)), std::move(owner));
        }
        accepted["taskIds"] = filter["taskIds"];
    }
    auto sub = std::make_shared<Sub>();
    sub->id = request["id"];
    sub->filter = std::move(accepted);
    sub->attributes = std::move(trusted_attributes);
    sub->task_owners = std::move(task_owners);
    sub->send = std::move(send);
    std::uint64_t key;
    std::unique_lock sublock(sub->mutex);
    {
        std::unique_lock lock(impl_->mutex);
        need(impl_->subscriptions.size() < impl_->options.max_subscriptions,
             "Too many subscriptions");
        key = impl_->next_subscription++;
        impl_->subscriptions.emplace(key, sub);
    }
    try {
        sub->send(make_notification(
            "notifications/subscriptions/acknowledged",
            {{"notifications", sub->filter}, {"_meta", {{subscription_key, sub->id}}}}));
    } catch (...) {
        std::unique_lock lock(impl_->mutex);
        impl_->subscriptions.erase(key);
        throw;
    }
    std::weak_ptr<Impl> weak = impl_;
    return std::shared_ptr<Subscription>(new Subscription([weak, sub, key] {
        std::lock_guard lock(sub->mutex);
        const bool active = std::exchange(sub->active, false);
        if (auto impl = weak.lock()) {
            JsonScope scope(impl->options.memory_resource);
            {
                std::unique_lock registry_lock(impl->mutex);
                impl->subscriptions.erase(key);
            }
            if (active)
                sub->send({{"jsonrpc", "2.0"},
                           {"id", sub->id},
                           {"result", impl->stamp({{"_meta", {{subscription_key, sub->id}}}})}});
        }
    }));
}
void Server::resource_updated(std::string_view uri) {
    JsonScope scope(impl_->options.memory_resource);
    impl_->publish("notifications/resources/updated", {{"uri", uri}});
}
void Server::notify(std::string_view method, Json params, std::string_view task_owner) {
    JsonScope scope(impl_->options.memory_resource);
    if (method != "notifications/tasks" || !params.is_object() ||
        !params.contains("taskId") || !params["taskId"].is_string() ||
        text(params["taskId"]).empty() || task_owner.empty())
        throw std::invalid_argument("Invalid Tasks notification");
    impl_->publish(method, std::move(params), task_owner);
}
} // namespace mcp
