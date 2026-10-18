#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <random>
#include <regex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <mcp/extensions.h>
#include "deadline.h"

namespace mcp::extensions {
namespace {
[[noreturn]] void invalid(std::string message) {
    throw RpcError(ErrorCode::invalid_params, std::move(message));
}
std::string
timestamp(std::chrono::system_clock::time_point value = std::chrono::system_clock::now()) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch());
    const auto seconds_duration = std::chrono::duration_cast<std::chrono::seconds>(ms);
    const auto fraction = (ms - seconds_duration).count();
    const auto seconds = static_cast<std::time_t>(seconds_duration.count());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
        << fraction << 'Z';
    return out.str();
}
std::optional<std::chrono::steady_clock::time_point>
expiry_for(const Json &wire, std::chrono::steady_clock::time_point created) {
    const auto ttl = wire["ttlMs"];
    if (ttl.is_null())
        return std::nullopt;
    const auto millis = ttl.get<std::int64_t>();
    const auto room = std::chrono::steady_clock::time_point::max() - created;
    const auto max_millis = std::chrono::duration_cast<std::chrono::milliseconds>(room).count();
    if (millis >= max_millis)
        return std::chrono::steady_clock::time_point::max();
    return created + std::chrono::milliseconds(millis);
}
std::string make_id() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("Cryptographic task ID generation failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}
std::string owner_for(const Json &attributes, const TaskServerOptions &options) {
    if (options.owner) {
        auto owner = options.owner(attributes);
        if (owner.empty())
            throw RpcError(ErrorCode::invalid_params,
                           "Task owner resolver returned an empty identity");
        return owner;
    }
    if (!attributes.is_object() || !attributes.contains("iss") || !attributes["iss"].is_string() ||
        !attributes.contains("sub") || !attributes["sub"].is_string() ||
        text(attributes["iss"]).empty() || text(attributes["sub"]).empty())
        throw RpcError(ErrorCode::missing_capability,
                       "Task operations require trusted stable issuer and subject attributes");
    return std::string(text(attributes["iss"])) + '\n' + std::string(text(attributes["sub"]));
}
bool has_tasks(const Json &caps) {
    return caps.is_object() && caps.contains("extensions") && caps["extensions"].is_object() &&
           caps["extensions"].contains(tasks_id) && caps["extensions"][tasks_id].is_object();
}
std::string task_id(const Json &params) {
    if (!params.is_object() || !params.contains("taskId") || !params["taskId"].is_string() ||
        text(params["taskId"]).empty())
        invalid("taskId must be a non-empty string");
    return std::string(text(params["taskId"]));
}
std::string digest(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), hash.data(), &size, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "sha256:";
    for (unsigned i = 0; i < size; ++i) {
        result.push_back(hex[hash[i] >> 4]);
        result.push_back(hex[hash[i] & 15]);
    }
    return result;
}
std::string read_bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Unable to read skill resource");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string mime_for(const std::filesystem::path &path) {
    const auto ext = path.extension().string();
    if (ext == ".md")
        return "text/markdown";
    if (ext == ".html" || ext == ".htm")
        return "text/html";
    if (ext == ".json")
        return "application/json";
    if (ext == ".txt")
        return "text/plain";
    if (ext == ".png")
        return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
    if (ext == ".svg")
        return "image/svg+xml";
    if (ext == ".pdf")
        return "application/pdf";
    return "application/octet-stream";
}
std::string unquote_frontmatter(std::string value) {
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\'')))
        return value.substr(1, value.size() - 2);
    return value;
}
std::string_view frontmatter_body(std::string_view markdown) {
    if (!markdown.starts_with("---\n") && !markdown.starts_with("---\r\n"))
        throw std::runtime_error("SKILL.md must begin with YAML frontmatter");
    const auto begin = markdown.find('\n') + 1;
    const auto end = markdown.find("\n---", begin);
    if (end == std::string_view::npos)
        throw std::runtime_error("SKILL.md frontmatter is not closed");
    return markdown.substr(begin, end - begin);
}
Json parse_frontmatter(std::string_view markdown) {
    if (!markdown.starts_with("---\n") && !markdown.starts_with("---\r\n"))
        throw std::runtime_error("SKILL.md must begin with YAML frontmatter");
    const auto begin = markdown.find('\n') + 1;
    auto end = markdown.find("\n---", begin);
    if (end == std::string_view::npos)
        throw std::runtime_error("SKILL.md frontmatter is not closed");
    Json result = Json::object();
    std::size_t pos = begin;
    while (pos < end) {
        auto line_end = markdown.find('\n', pos);
        if (line_end == std::string_view::npos || line_end > end)
            line_end = end;
        auto line = markdown.substr(pos, line_end - pos);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            auto key = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            if ((key == "name" || key == "description") && !value.empty())
                result[std::string(key)] = unquote_frontmatter(std::string(value));
        }
        pos = line_end + 1;
    }
    if (!result.contains("name") || !result.contains("description"))
        throw std::runtime_error("SKILL.md frontmatter requires name and description");
    return result;
}
std::string safe_uri_relative(std::string_view uri, std::string_view prefix) {
    if (!uri.starts_with(prefix))
        invalid("Skill URI is outside this provider");
    uri.remove_prefix(prefix.size());
    if (uri.empty() || uri.find('%') != std::string_view::npos ||
        uri.find('\\') != std::string_view::npos || uri.find('\0') != std::string_view::npos)
        invalid("Invalid skill URI path");
    std::filesystem::path relative{std::string(uri)};
    if (relative.is_absolute())
        invalid("Absolute skill paths are forbidden");
    for (const auto &part : relative)
        if (part == "." || part == "..")
            invalid("Skill path traversal is forbidden");
    return relative.generic_string();
}
void check_nonempty_string(const Json &object, std::string_view key) {
    if (!object.contains(key) || !object[key].is_string() || text(object[key]).empty())
        invalid(std::string(key) + " must be a non-empty string");
}
} // namespace

std::string_view to_string(TaskStatus status) noexcept {
    switch (status) {
    case TaskStatus::working:
        return "working";
    case TaskStatus::input_required:
        return "input_required";
    case TaskStatus::completed:
        return "completed";
    case TaskStatus::cancelled:
        return "cancelled";
    case TaskStatus::failed:
        return "failed";
    }
    return "failed";
}
TaskStatus task_status(std::string_view value) {
    for (auto status : {TaskStatus::working, TaskStatus::input_required, TaskStatus::completed,
                        TaskStatus::cancelled, TaskStatus::failed})
        if (to_string(status) == value)
            return status;
    invalid("Unknown task status");
}

void validate_task(const Json &task, bool create_result) {
    const auto status =
        task_status(task.is_object() && task.contains("status") && task["status"].is_string()
                        ? text(task["status"])
                        : std::string_view{});
    check_nonempty_string(task, "taskId");
    static const std::regex rfc3339_ms(
        R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$)");
    for (auto key : {"createdAt", "lastUpdatedAt"}) {
        check_nonempty_string(task, key);
        if (!std::regex_match(std::string(text(task[key])), rfc3339_ms))
            invalid(std::string(key) + " must be an RFC3339 timestamp with milliseconds");
    }
    if (!task.contains("ttlMs") ||
        !(task["ttlMs"].is_number_unsigned() || task["ttlMs"].is_number_integer() ||
          task["ttlMs"].is_null()) ||
        (task["ttlMs"].is_number_integer() && !task["ttlMs"].is_number_unsigned() &&
         task["ttlMs"].get<std::int64_t>() < 0) ||
        (task["ttlMs"].is_number_unsigned() &&
         task["ttlMs"].get<std::uint64_t>() >
             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        invalid("ttlMs must be a non-negative integer or null");
    for (auto key : {"statusMessage"})
        if (task.contains(key) && !task[key].is_string())
            invalid(std::string(key) + " must be a string");
    if (task.contains("pollIntervalMs") &&
        (!task["pollIntervalMs"].is_number_integer() ||
         (task["pollIntervalMs"].is_number_unsigned() &&
          task["pollIntervalMs"].get<std::uint64_t>() >
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) ||
         (!task["pollIntervalMs"].is_number_unsigned() &&
          task["pollIntervalMs"].get<std::int64_t>() < 0)))
        invalid("pollIntervalMs must be non-negative");
    if (status == TaskStatus::input_required &&
        (!task.contains("inputRequests") || !task["inputRequests"].is_object()))
        invalid("input_required task needs inputRequests");
    if (status == TaskStatus::completed && !task.contains("result"))
        invalid("completed task needs result");
    if (status == TaskStatus::failed && (!task.contains("error") || !task["error"].is_object()))
        invalid("failed task needs a JSON-RPC error");
    if (create_result && (!task.contains("resultType") || task["resultType"] != "task"))
        invalid("CreateTaskResult requires resultType task");
}

Json task_result(const TaskRecord &record) {
    Json result = record.wire;
    result["resultType"] = "complete";
    validate_task(result);
    return result;
}
Json create_task_result(const TaskRecord &record) {
    Json result = record.wire;
    result["resultType"] = "task";
    validate_task(result, true);
    return result;
}
TaskRecord make_task(std::string owner, std::string id, TaskStatus status,
                     std::chrono::milliseconds ttl, std::chrono::milliseconds poll,
                     std::string message) {
    if (owner.empty() || id.empty() || ttl.count() < -1 || poll.count() < 0)
        throw std::invalid_argument("Invalid task identity or timing options");
    TaskRecord task;
    task.owner = std::move(owner);
    task.wire = {{"taskId", std::move(id)},
                 {"status", to_string(status)},
                 {"createdAt", timestamp()},
                 {"lastUpdatedAt", timestamp()},
                 {"ttlMs", ttl.count() < 0 ? Json(nullptr) : Json(ttl.count())},
                 {"pollIntervalMs", poll.count()}};
    if (!message.empty())
        task.wire["statusMessage"] = std::move(message);
    if (status == TaskStatus::input_required)
        task.wire["inputRequests"] = Json::object();
    if (status == TaskStatus::completed)
        task.wire["result"] = Json::object();
    if (status == TaskStatus::failed)
        task.wire["error"] = {{"code", -32603}, {"message", "Task failed"}};
    validate_task(task.wire);
    return task;
}
TaskRecord create_task(RequestContext &context, TaskStore &store, const TaskServerOptions &options,
                       std::chrono::milliseconds ttl, std::chrono::milliseconds poll,
                       std::string message) {
    if (!has_tasks(context.capabilities()))
        throw RpcError(ErrorCode::missing_capability, "Tasks extension capability is required",
                       {{"requiredCapabilities",
                         {{"extensions", {{std::string(tasks_id), Json::object()}}}}}});
    return store.create(make_task(owner_for(context.attributes(), options), make_id(),
                                  TaskStatus::working, ttl, poll, std::move(message)));
}
TaskRecord create_task(RequestContext &context, TaskStore &store, const TaskServerOptions &options,
                       std::string message) {
    return create_task(context, store, options, options.default_ttl, options.poll_interval,
                       std::move(message));
}
void transition_task(TaskRecord &task, TaskStatus status, Json payload) {
    const auto previous = task_status(text(task.wire["status"]));
    if (previous == TaskStatus::completed || previous == TaskStatus::cancelled ||
        previous == TaskStatus::failed)
        throw std::logic_error("Terminal task status cannot transition");
    if (!payload.is_object())
        throw std::invalid_argument("Task transition payload must be an object");
    TaskRecord candidate = task;
    for (auto key : {"inputRequests", "result", "error"})
        candidate.wire.erase(key);
    if (status == TaskStatus::input_required) {
        if (!payload.contains("inputRequests") || !payload["inputRequests"].is_object() ||
            payload["inputRequests"].empty())
            throw std::invalid_argument("input_required transition needs non-empty inputRequests");
        candidate.wire["inputRequests"] = payload["inputRequests"];
        for (auto it = payload["inputRequests"].begin(); it != payload["inputRequests"].end(); ++it)
            if (candidate.answered_input_keys.contains(std::string(it.key())) ||
                !it.value().is_object() || !it.value().contains("method") ||
                !it.value()["method"].is_string() || !it.value().contains("params") ||
                !it.value()["params"].is_object())
                throw std::invalid_argument("Malformed task input request");
    } else if (status == TaskStatus::completed) {
        if (!payload.contains("result") || !payload["result"].is_object())
            throw std::invalid_argument("completed transition needs an object result");
        candidate.wire["result"] = payload["result"];
    } else if (status == TaskStatus::failed) {
        if (!payload.contains("error") || !payload["error"].is_object() ||
            !payload["error"].contains("code") || !payload["error"].contains("message"))
            throw std::invalid_argument("failed transition needs a JSON-RPC error");
        candidate.wire["error"] = payload["error"];
    }
    candidate.wire["status"] = to_string(status);
    candidate.wire["lastUpdatedAt"] = timestamp();
    validate_task(candidate.wire);
    task = std::move(candidate);
}
void request_task_cancel(TaskRecord &task) {
    if (task_status(text(task.wire["status"])) == TaskStatus::completed ||
        task_status(text(task.wire["status"])) == TaskStatus::cancelled ||
        task_status(text(task.wire["status"])) == TaskStatus::failed)
        return;
    task.cancel_requested = true;
    task.wire["lastUpdatedAt"] = timestamp();
}
void accept_task_inputs(TaskRecord &task, const Json &responses) {
    if (task_status(text(task.wire["status"])) != TaskStatus::input_required ||
        !responses.is_object())
        invalid("Task is not waiting for input or inputResponses is invalid");
    TaskRecord candidate = task;
    const auto pending = candidate.wire["inputRequests"];
    for (auto it = responses.begin(); it != responses.end(); ++it) {
        if (!pending.contains(it.key()))
            continue; // Spec: unknown/already answered keys are ignored.
        if (!it.value().is_object())
            invalid("Input response must be an object");
        const auto method = text(pending[it.key()]["method"]);
        if (method == "elicitation/create") {
            if (!it.value().contains("action") ||
                (it.value()["action"] != "accept" && it.value()["action"] != "decline" &&
                 it.value()["action"] != "cancel") ||
                (it.value().contains("content") && !it.value()["content"].is_object()))
                invalid("Invalid elicitation input response");
        } else if (method == "roots/list") {
            if (!it.value().contains("roots") || !it.value()["roots"].is_array())
                invalid("Invalid roots input response");
        } else if (method == "sampling/createMessage") {
            if (!it.value().contains("model") || !it.value()["model"].is_string() ||
                !it.value().contains("role") || !it.value()["role"].is_string() ||
                !it.value().contains("content"))
                invalid("Invalid sampling input response");
        } else
            invalid("Unsupported task input request method");
        const std::string response_key(it.key());
        candidate.input_responses[response_key] = it.value();
        candidate.answered_input_keys.insert(response_key);
        candidate.wire["inputRequests"].erase(response_key);
    }
    candidate.wire["lastUpdatedAt"] = timestamp();
    if (candidate.wire["inputRequests"].empty()) {
        candidate.wire.erase("inputRequests");
        candidate.wire["status"] = "working";
    }
    validate_task(candidate.wire);
    task = std::move(candidate);
}

MemoryTaskStore::MemoryTaskStore(std::size_t max_tasks) : max_tasks_(max_tasks) {
    if (!max_tasks_)
        throw std::invalid_argument("Task store capacity must be positive");
}
std::size_t MemoryTaskStore::prune_expired_locked(std::chrono::steady_clock::time_point now) {
    std::size_t removed = 0;
    while (!expirations_.empty() && expirations_.begin()->first <= now) {
        auto expiry = expirations_.begin();
        const auto task = tasks_.find(expiry->second);
        if (task != tasks_.end()) {
            tasks_.erase(task);
            ++removed;
        }
        expirations_.erase(expiry);
    }
    return removed;
}
std::size_t MemoryTaskStore::prune_expired() {
    std::lock_guard lock(mutex_);
    return prune_expired_locked(std::chrono::steady_clock::now());
}
TaskRecord MemoryTaskStore::create(TaskRecord task) {
    if (task.owner.empty())
        throw std::invalid_argument("Task owner is required");
    validate_task(task.wire);
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    prune_expired_locked(now);
    auto key = std::make_pair(task.owner, std::string(text(task.wire["taskId"])));
    if (tasks_.contains(key))
        throw std::logic_error("Duplicate task ID for owner");
    if (tasks_.size() >= max_tasks_)
        throw std::length_error("Task store capacity exceeded");
    task.revision = 1;
    task.memory_created = now;
    task.memory_expiry = expiry_for(task.wire, *task.memory_created);
    std::optional<ExpiryIndex::iterator> expiry;
    if (task.memory_expiry)
        expiry = expirations_.emplace(*task.memory_expiry, key);
    try {
        tasks_.emplace(std::move(key), StoredTask{task, expiry});
    } catch (...) {
        if (expiry)
            expirations_.erase(*expiry);
        throw;
    }
    return task;
}
std::optional<TaskRecord> MemoryTaskStore::get(std::string_view owner, std::string_view id) {
    std::lock_guard lock(mutex_);
    prune_expired_locked(std::chrono::steady_clock::now());
    auto it = tasks_.find({std::string(owner), std::string(id)});
    if (it == tasks_.end())
        return {};
    return it->second.record;
}
bool MemoryTaskStore::compare_exchange(std::string_view owner, std::string_view id,
                                       std::uint64_t expected, TaskRecord replacement) {
    std::lock_guard lock(mutex_);
    prune_expired_locked(std::chrono::steady_clock::now());
    auto it = tasks_.find({std::string(owner), std::string(id)});
    if (it == tasks_.end())
        return false;
    if (it->second.record.revision != expected)
        return false;
    validate_task(replacement.wire);
    if (text(replacement.wire["taskId"]) != id ||
        replacement.wire["createdAt"] != it->second.record.wire["createdAt"])
        throw std::invalid_argument("Task identity and creation time are immutable");
    if (task_status(text(it->second.record.wire["status"])) == TaskStatus::completed ||
        task_status(text(it->second.record.wire["status"])) == TaskStatus::failed ||
        task_status(text(it->second.record.wire["status"])) == TaskStatus::cancelled)
        throw std::logic_error("Terminal task status cannot be changed");
    replacement.owner = std::string(owner);
    replacement.revision = expected + 1;
    replacement.memory_created = it->second.record.memory_created;
    if (!replacement.memory_created)
        replacement.memory_created = std::chrono::steady_clock::now();
    replacement.memory_expiry = expiry_for(replacement.wire, *replacement.memory_created);
    std::optional<ExpiryIndex::iterator> expiry;
    if (replacement.memory_expiry)
        expiry = expirations_.emplace(*replacement.memory_expiry, it->first);
    // TaskRecord consists of nothrow-movable owning values, so an expiry-index
    // allocation failure above cannot partially commit the replacement.
    static_assert(std::is_nothrow_move_assignable_v<TaskRecord>);
    const auto old_expiry = it->second.expiry;
    it->second.record = std::move(replacement);
    it->second.expiry = expiry;
    if (old_expiry)
        expirations_.erase(*old_expiry);
    return true;
}
bool MemoryTaskStore::erase(std::string_view owner, std::string_view id, std::uint64_t expected) {
    std::lock_guard lock(mutex_);
    prune_expired_locked(std::chrono::steady_clock::now());
    auto it = tasks_.find({std::string(owner), std::string(id)});
    if (it == tasks_.end() || it->second.record.revision != expected)
        return false;
    if (it->second.expiry)
        expirations_.erase(*it->second.expiry);
    tasks_.erase(it);
    return true;
}

void install_tasks(Server &server, std::shared_ptr<TaskStore> store, TaskServerOptions options) {
    if (!store || options.default_ttl.count() < 0 || options.poll_interval.count() < 0)
        throw std::invalid_argument("Invalid Tasks extension options");
    const Json settings = Json::object();
    auto require = [](RequestContext &ctx) {
        if (!has_tasks(ctx.capabilities()))
            throw RpcError(ErrorCode::missing_capability, "Tasks extension capability is required",
                           {{"requiredCapabilities",
                             {{"extensions", {{std::string(tasks_id), Json::object()}}}}}});
    };
    server.add_extension(std::string(tasks_id), settings, "tasks/get",
                         [store, options, require](const Json &params, RequestContext &ctx) {
                             require(ctx);
                             auto task =
                                 store->get(owner_for(ctx.attributes(), options), task_id(params));
                             if (!task)
                                 invalid("Task not found");
                             return task_result(*task);
                         });
    server.add_extension(
        std::string(tasks_id), settings, "tasks/update",
        [store, options, require](const Json &params, RequestContext &ctx) {
            require(ctx);
            auto owner = owner_for(ctx.attributes(), options);
            const auto id = task_id(params);
            if (!params.contains("inputResponses") || !params["inputResponses"].is_object())
                invalid("inputResponses must be an object");
            auto task = store->get(owner, id);
            if (!task)
                invalid("Task not found");
            auto updated = *task;
            accept_task_inputs(updated, params["inputResponses"]);
            if (!store->compare_exchange(owner, id, task->revision, std::move(updated)))
                throw RpcError(ErrorCode::internal_error,
                               "Task changed concurrently; retry update");
            return Json::object();
        });
    server.add_extension(
        std::string(tasks_id), settings, "tasks/cancel",
        [store, options, require](const Json &params, RequestContext &ctx) {
            require(ctx);
            auto owner = owner_for(ctx.attributes(), options);
            const auto id = task_id(params);
            auto task = store->get(owner, id);
            if (!task)
                invalid("Task not found");
            const auto status = task_status(text(task->wire["status"]));
            if (status == TaskStatus::completed || status == TaskStatus::failed ||
                status == TaskStatus::cancelled)
                return Json::object();
            auto updated = *task;
            request_task_cancel(updated);
            if (!store->compare_exchange(owner, id, task->revision, std::move(updated)))
                throw RpcError(ErrorCode::internal_error,
                               "Task changed concurrently; retry cancel");
            if (options.on_cancel)
                options.on_cancel(owner, id);
            return Json::object();
        });
    server.set_task_subscription_authorizer(
        [store, options](const Json &attributes, std::string_view id) {
            auto owner = owner_for(attributes, options);
            if (!store->get(owner, id))
                invalid("Task not found");
            return owner;
        });
}
Json tasks_capability() {
    return {{"extensions", {{std::string(tasks_id), Json::object()}}}};
}

Json TaskClient::call_tool(std::string_view name, Json args, RequestOptions options,
                           NotificationHandler notify) {
    return client_.call_tool(name, std::move(args), std::move(options), std::move(notify));
}
Json TaskClient::run_tool(std::string_view name, Json args, InputHandler input,
                          RequestOptions options, NotificationHandler notify) {
    if (options.timeout <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("Task timeout must be positive");
    const auto deadline = detail::request_deadline(options.timeout);
    auto remaining_options = [&] {
        if (options.stop.stop_requested())
            throw TransportError(TransportErrorKind::cancelled, "Task wait cancelled");
        const auto remaining = detail::remaining_time(deadline);
        if (remaining <= std::chrono::milliseconds::zero())
            throw TransportError(TransportErrorKind::timeout, "Task wait timed out");
        auto call_options = options;
        call_options.timeout = remaining;
        return call_options;
    };
    Json result = call_tool(name, std::move(args), remaining_options(), notify);
    (void)remaining_options();
    if (result.value("resultType", Json("complete")) != "task")
        return result;
    validate_task(result, true);
    return run(text(result["taskId"]), std::move(input), remaining_options(), std::move(notify));
}
Json TaskClient::get(std::string_view id, RequestOptions options) {
    Json result = client_.request("tasks/get", {{"taskId", id}}, std::move(options));
    validate_task(result);
    return result;
}
Json TaskClient::update(std::string_view id, Json responses, RequestOptions options) {
    if (!responses.is_object())
        throw std::invalid_argument("inputResponses must be an object");
    return client_.request("tasks/update",
                           {{"taskId", id}, {"inputResponses", std::move(responses)}},
                           std::move(options));
}
Json TaskClient::cancel(std::string_view id, RequestOptions options) {
    return client_.request("tasks/cancel", {{"taskId", id}}, std::move(options));
}
Json TaskClient::run(std::string_view id, InputHandler input, RequestOptions options,
                     NotificationHandler notify) {
    if (options.timeout <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("Task timeout must be positive");
    const auto deadline = detail::request_deadline(options.timeout);
    auto remaining_options = [&] {
        if (options.stop.stop_requested())
            throw TransportError(TransportErrorKind::cancelled, "Task wait cancelled");
        const auto remaining = detail::remaining_time(deadline);
        if (remaining <= std::chrono::milliseconds::zero())
            throw TransportError(TransportErrorKind::timeout, "Task wait timed out");
        auto call_options = options;
        call_options.timeout = remaining;
        return call_options;
    };
    for (;;) {
        Json task = get(id, remaining_options());
        (void)remaining_options();
        const auto status = task_status(text(task["status"]));
        if (status == TaskStatus::completed)
            return task["result"];
        if (status == TaskStatus::failed) {
            const auto &error = task["error"];
            throw RpcError(error.value("code", -32603),
                           std::string(text(error.value("message", Json("Task failed")))),
                           error.value("data", Json()));
        }
        if (status == TaskStatus::cancelled)
            return task;
        if (status == TaskStatus::input_required) {
            if (!input)
                throw std::runtime_error("Task requires client input");
            Json responses = Json::object();
            for (auto it = task["inputRequests"].begin(); it != task["inputRequests"].end(); ++it) {
                (void)remaining_options();
                responses[it.key()] = input(it.value());
            }
            update(id, std::move(responses), remaining_options());
        }
        const auto wait = task.value("pollIntervalMs", std::int64_t{1000});
        if (options.stop.stop_requested())
            throw TransportError(TransportErrorKind::cancelled, "Task wait cancelled");
        (void)notify; // Task status callbacks use the core subscriptions/listen stream.
        const auto remaining = detail::remaining_time(deadline);
        const auto pause = std::min(std::chrono::milliseconds(wait), remaining);
        if (pause > std::chrono::milliseconds::zero()) {
            std::mutex wait_mutex;
            std::condition_variable_any wait_condition;
            std::unique_lock wait_lock(wait_mutex);
            wait_condition.wait_for(wait_lock, options.stop, pause, [] { return false; });
        }
    }
}

SkillDirectory::SkillDirectory(std::filesystem::path root, std::string prefix,
                               FrontmatterParser parser, SkillListOptions list_options)
    : root_(std::filesystem::canonical(std::move(root))), prefix_(std::move(prefix)),
      frontmatter_parser_(std::move(parser)), list_options_(std::move(list_options)) {
    if (prefix_.empty() || prefix_.back() != '/' || prefix_.find(':') == std::string::npos)
        throw std::invalid_argument(
            "Skill URI prefix must be an absolute scheme prefix ending in slash");
    if (list_options_.page_size == 0 || list_options_.page_size > 10000 ||
        list_options_.ttl_ms >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        (list_options_.cache_scope != "private" && list_options_.cache_scope != "public"))
        throw std::invalid_argument("Invalid Skills list paging or cache options");
}
std::filesystem::path SkillDirectory::safe_path(std::string_view relative) const {
    auto path = root_ / std::filesystem::path(std::string(relative));
    auto canonical = std::filesystem::weakly_canonical(path);
    auto root_it = root_.begin(), path_it = canonical.begin();
    for (; root_it != root_.end(); ++root_it, ++path_it)
        if (path_it == canonical.end() || *root_it != *path_it)
            invalid("Skill path escapes configured directory");
    auto cur = root_;
    for (const auto &part : std::filesystem::path(std::string(relative))) {
        cur /= part;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(cur)))
            invalid("Skill provider does not follow symbolic links");
    }
    return canonical;
}
Json SkillDirectory::entry(const std::filesystem::path &dir) const {
    auto skill_path = std::filesystem::relative(dir, root_).generic_string();
    auto md = safe_path(skill_path + "/SKILL.md");
    if (!std::filesystem::is_regular_file(md))
        invalid("Skill has no SKILL.md");
    const auto bytes = read_bytes(md);
    auto frontmatter = frontmatter_parser_ ? frontmatter_parser_(frontmatter_body(bytes))
                                           : parse_frontmatter(bytes);
    if (!frontmatter.is_object() || !frontmatter.contains("name") ||
        !frontmatter["name"].is_string() || !frontmatter.contains("description") ||
        !frontmatter["description"].is_string())
        invalid("Skill frontmatter parser must return name and description strings");
    if (skill_path.substr(skill_path.find_last_of('/') == std::string::npos
                              ? 0
                              : skill_path.find_last_of('/') + 1) != text(frontmatter["name"]))
        invalid("Skill directory name must match frontmatter name");
    std::string base = prefix_ + skill_path + "/";
    Json resources = Json::array();
    for (const auto &item : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied)) {
        if (item.is_symlink())
            continue;
        if (!item.is_regular_file())
            continue;
        const auto relative = std::filesystem::relative(item.path(), dir).generic_string();
        const auto content = read_bytes(safe_path(skill_path + "/" + relative));
        resources.push_back(
            {{"uri", base + relative}, {"digest", digest(content)}, {"size", content.size()}});
    }
    std::sort(resources.begin(), resources.end(),
              [](const Json &a, const Json &b) { return text(a["uri"]) < text(b["uri"]); });
    return {{"uri", base + "SKILL.md"},
            {"frontmatter", std::move(frontmatter)},
            {"resources", std::move(resources)}};
}
Json SkillDirectory::list(std::string_view cursor) const {
    Json skills = Json::array();
    for (const auto &item : std::filesystem::recursive_directory_iterator(
             root_, std::filesystem::directory_options::skip_permission_denied)) {
        if (item.is_symlink() || !item.is_directory())
            continue;
        auto md = item.path() / "SKILL.md";
        if (!std::filesystem::is_regular_file(md))
            continue;
        try {
            skills.push_back(entry(item.path()));
        } catch (const RpcError &) {
            throw;
        }
    }
    std::sort(skills.begin(), skills.end(),
              [](const Json &a, const Json &b) { return text(a["uri"]) < text(b["uri"]); });
    const auto fingerprint = digest(skills.dump());
    std::size_t offset = 0;
    if (!cursor.empty()) {
        const auto first = cursor.find(';');
        const auto second = first == std::string_view::npos ? first : cursor.find(';', first + 1);
        if (first == std::string_view::npos || second == std::string_view::npos ||
            cursor.substr(0, first) != "skills" ||
            cursor.substr(first + 1, second - first - 1) != fingerprint)
            invalid("Invalid or stale skills/list cursor");
        const auto number = cursor.substr(second + 1);
        const auto [end, error] =
            std::from_chars(number.data(), number.data() + number.size(), offset);
        if (number.empty() || error != std::errc{} || end != number.data() + number.size() ||
            offset == 0 || offset >= skills.size())
            invalid("Invalid skills/list cursor offset");
    }
    const auto end = std::min(skills.size(), offset + list_options_.page_size);
    Json page = Json::array();
    for (auto i = offset; i < end; ++i)
        page.push_back(std::move(skills[i]));
    Json result = {{"resultType", "complete"},
                   {"skills", std::move(page)},
                   {"ttlMs", list_options_.ttl_ms},
                   {"cacheScope", list_options_.cache_scope}};
    if (end < skills.size())
        result["nextCursor"] = "skills;" + fingerprint + ";" + std::to_string(end);
    return result;
}
Json SkillDirectory::get(std::string_view uri) const {
    auto rel = safe_uri_relative(uri, prefix_);
    if (!rel.ends_with("/SKILL.md"))
        invalid("skills/get URI must identify SKILL.md");
    auto dir = safe_path(std::filesystem::path(rel).parent_path().generic_string());
    if (!std::filesystem::is_directory(dir))
        invalid("Unknown skill");
    return {
        {"resultType", "complete"}, {"skill", entry(dir)}, {"ttlMs", 0}, {"cacheScope", "private"}};
}
Json SkillDirectory::read(std::string_view uri) const {
    auto rel = safe_uri_relative(uri, prefix_);
    auto path = safe_path(rel);
    if (!std::filesystem::is_regular_file(path))
        invalid("Unknown skill resource");
    auto bytes = read_bytes(path);
    Json content = {{"uri", uri}, {"mimeType", mime_for(path)}};
    const auto mime = mime_for(path);
    if (mime.starts_with("text/") || mime.find("json") != std::string::npos ||
        mime.find("svg") != std::string::npos) {
        content["text"] = bytes;
    } else {
        std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
        const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                         reinterpret_cast<const unsigned char *>(bytes.data()),
                                         static_cast<int>(bytes.size()));
        if (size < 0)
            throw std::runtime_error("Base64 encoding failed");
        encoded.resize(static_cast<std::size_t>(size));
        content["blob"] = std::move(encoded);
    }
    return {{"resultType", "complete"},
            {"contents", Json::array({std::move(content)})},
            {"ttlMs", 0},
            {"cacheScope", "private"}};
}
Json SkillDirectory::read_directory(std::string_view uri) const {
    auto rel = safe_uri_relative(uri, prefix_);
    auto dir = safe_path(rel);
    if (!std::filesystem::is_directory(dir))
        invalid("Unknown skill directory");
    Json resources = Json::array();
    for (const auto &item : std::filesystem::directory_iterator(dir)) {
        if (item.is_symlink())
            continue;
        const auto child_rel =
            (std::filesystem::path(rel) / item.path().filename()).generic_string();
        resources.push_back(
            {{"uri", prefix_ + child_rel},
             {"name", item.path().filename().string()},
             {"mimeType", item.is_directory() ? "inode/directory" : mime_for(item.path())}});
    }
    return {{"resultType", "complete"}, {"resources", std::move(resources)}};
}
Json SkillDirectory::resource_descriptors() const {
    Json resources = Json::array();
    std::string cursor;
    for (;;) {
        const auto page = list(cursor);
        for (const auto &skill : page["skills"])
            for (const auto &file : skill["resources"])
                resources.push_back(
                    {{"uri", file["uri"]},
                     {"name",
                      std::filesystem::path(std::string(text(file["uri"]))).filename().string()},
                     {"mimeType",
                      mime_for(std::filesystem::path(std::string(text(file["uri"]))))}});
        if (!page.contains("nextCursor"))
            break;
        cursor = std::string(text(page["nextCursor"]));
    }
    return resources;
}
Json skills_capability(bool directory_read) {
    return {
        {std::string(skills_id), directory_read ? Json{{"directoryRead", true}} : Json::object()}};
}
void install_skills(Server &server, std::shared_ptr<SkillDirectory> provider, bool directory_read) {
    if (!provider)
        throw std::invalid_argument("SkillDirectory provider is required");
    const Json settings = directory_read ? Json{{"directoryRead", true}} : Json::object();
    server.add_extension(std::string(skills_id), settings, "skills/list",
                         [provider](const Json &params, RequestContext &) {
                             if (!params.is_object())
                                 invalid("skills/list params must be an object");
                             if (params.contains("cursor") && !params["cursor"].is_string())
                                 invalid("cursor must be a string");
                             return provider->list(params.contains("cursor")
                                                       ? text(params["cursor"])
                                                       : std::string_view{});
                         });
    server.add_extension(std::string(skills_id), settings, "skills/get",
                         [provider](const Json &params, RequestContext &) {
                             if (!params.contains("uri") || !params["uri"].is_string())
                                 invalid("uri is required");
                             return provider->get(text(params["uri"]));
                         });
    server.set_resource_resolver([provider](const Json &params, RequestContext &) {
        if (!params.contains("uri") || !params["uri"].is_string())
            invalid("uri is required");
        return provider->read(text(params["uri"]));
    });
    for (const auto &descriptor : provider->resource_descriptors()) {
        Resource resource;
        resource.uri = std::string(text(descriptor["uri"]));
        resource.name = std::string(text(descriptor["name"]));
        resource.mime_type = std::string(text(descriptor["mimeType"]));
        server.add_resource(std::move(resource), [provider](const Json &params, RequestContext &) {
            return provider->read(text(params["uri"]));
        });
    }
    if (directory_read)
        server.add_extension(std::string(skills_id), settings, "resources/directory/read",
                             [provider](const Json &params, RequestContext &) {
                                 if (!params.contains("uri") || !params["uri"].is_string())
                                     invalid("uri is required");
                                 return provider->read_directory(text(params["uri"]));
                             });
}
Json SkillsClient::list(RequestOptions options) {
    return client_.list_result("skills/list", "skills", std::move(options),
                              [](const Json &page, const Json &first) {
        if (!page.contains("resultType") || page["resultType"] != "complete" ||
            !page.contains("skills") || !page["skills"].is_array() || !page.contains("ttlMs") ||
            !page["ttlMs"].is_number_integer() || !page.contains("cacheScope") ||
            !page["cacheScope"].is_string())
            throw TransportError(TransportErrorKind::invalid_response,
                                 "Invalid skills/list result");
        if ((page["ttlMs"].is_number_integer() && !page["ttlMs"].is_number_unsigned() &&
             page["ttlMs"].get<std::int64_t>() < 0) ||
            (page["cacheScope"] != "private" && page["cacheScope"] != "public"))
            throw TransportError(TransportErrorKind::invalid_response,
                                 "Invalid skills/list cache metadata");
        if (!first.is_null() && (first["ttlMs"] != page["ttlMs"] ||
                                first["cacheScope"] != page["cacheScope"])) {
            throw TransportError(TransportErrorKind::invalid_response,
                                 "Inconsistent Skills list cache metadata across pages");
        }
    });
}
Json SkillsClient::get(std::string_view uri, RequestOptions options) {
    Json result = client_.request("skills/get", {{"uri", uri}}, std::move(options));
    if (!result.contains("skill") || !result["skill"].is_object())
        throw TransportError(TransportErrorKind::invalid_response, "Invalid skills/get result");
    return result;
}
Json SkillsClient::read_directory(std::string_view uri, RequestOptions options) {
    Json result = client_.request("resources/directory/read", {{"uri", uri}}, std::move(options));
    if (!result.contains("resources") || !result["resources"].is_array())
        throw TransportError(TransportErrorKind::invalid_response,
                             "Invalid resources/directory/read result");
    return result;
}

std::string_view to_string(UiMethod method) noexcept {
    switch (method) {
    case UiMethod::open_link:
        return "ui/open-link";
    case UiMethod::message:
        return "ui/message";
    case UiMethod::initialize:
        return "ui/initialize";
    case UiMethod::initialized:
        return "ui/notifications/initialized";
    case UiMethod::update_model_context:
        return "ui/update-model-context";
    case UiMethod::request_display_mode:
        return "ui/request-display-mode";
    case UiMethod::resource_teardown:
        return "ui/resource-teardown";
    case UiMethod::request_teardown:
        return "ui/notifications/request-teardown";
    case UiMethod::tool_input:
        return "ui/notifications/tool-input";
    case UiMethod::tool_input_partial:
        return "ui/notifications/tool-input-partial";
    case UiMethod::tool_result:
        return "ui/notifications/tool-result";
    case UiMethod::tool_cancelled:
        return "ui/notifications/tool-cancelled";
    case UiMethod::size_changed:
        return "ui/notifications/size-changed";
    case UiMethod::host_context_changed:
        return "ui/notifications/host-context-changed";
    case UiMethod::sandbox_proxy_ready:
        return "ui/notifications/sandbox-proxy-ready";
    case UiMethod::sandbox_resource_ready:
        return "ui/notifications/sandbox-resource-ready";
    }
    return "";
}
std::string_view to_string(DraftUiMethod method) noexcept {
    switch (method) {
    case DraftUiMethod::download_file:
        return "ui/download-file";
    }
    return "";
}
Json ui_capability() {
    return {{std::string(apps_id), {{"mimeTypes", Json::array({apps_mime_type})}}}};
}
namespace {
void validate_ui_resource_meta(const Json &meta) {
    if (!meta.is_object())
        throw std::invalid_argument("MCP Apps resource metadata must be an object");
    if (meta.contains("csp")) {
        if (!meta["csp"].is_object())
            throw std::invalid_argument("MCP Apps csp must be an object");
        for (auto key : {"connectDomains", "resourceDomains", "frameDomains", "baseUriDomains"}) {
            if (!meta["csp"].contains(key))
                continue;
            if (!meta["csp"][key].is_array())
                throw std::invalid_argument("MCP Apps CSP domains must be arrays");
            for (const auto &domain : meta["csp"][key])
                if (!domain.is_string())
                    throw std::invalid_argument("MCP Apps CSP domains must be strings");
        }
    }
    if (meta.contains("permissions")) {
        if (!meta["permissions"].is_object())
            throw std::invalid_argument("MCP Apps permissions must be an object");
        for (auto key : {"camera", "microphone", "geolocation", "clipboardWrite"})
            if (meta["permissions"].contains(key) && !meta["permissions"][key].is_object())
                throw std::invalid_argument("MCP Apps permission flags must be empty objects");
    }
    if (meta.contains("domain") && !meta["domain"].is_string())
        throw std::invalid_argument("MCP Apps domain must be a string");
    if (meta.contains("prefersBorder") && !meta["prefersBorder"].is_boolean())
        throw std::invalid_argument("MCP Apps prefersBorder must be boolean");
}
} // namespace
Json ui_tool_metadata(std::string_view uri, std::vector<std::string> visibility) {
    if (!uri.starts_with("ui://") || uri.find('/') == std::string_view::npos)
        throw std::invalid_argument("MCP App resource URI must use ui://");
    Json scopes = Json::array();
    std::set<std::string> unique;
    for (auto &scope : visibility) {
        if ((scope != "model" && scope != "app") || !unique.insert(scope).second)
            throw std::invalid_argument("Tool visibility must contain unique model/app values");
        scopes.push_back(scope);
    }
    return {{"ui", {{"resourceUri", uri}, {"visibility", std::move(scopes)}}}};
}
Resource ui_resource(std::string uri, std::string name, Json metadata, std::string description) {
    if (!uri.starts_with("ui://") || !metadata.is_object())
        throw std::invalid_argument("Invalid MCP App resource");
    validate_ui_resource_meta(metadata);
    Resource resource{std::move(uri), std::move(name), std::move(description),
                      std::string(apps_mime_type)};
    resource.metadata = {{"ui", std::move(metadata)}};
    return resource;
}
Json ui_resource_contents(std::string_view uri, std::string html, Json metadata) {
    if (!uri.starts_with("ui://") || !metadata.is_object())
        throw std::invalid_argument("Invalid MCP App resource contents");
    validate_ui_resource_meta(metadata);
    return {{"resultType", "complete"},
            {"contents", Json::array({{{"uri", uri},
                                       {"mimeType", apps_mime_type},
                                       {"text", std::move(html)},
                                       {"_meta", {{"ui", std::move(metadata)}}}}})}};
}
Json ui_message(UiMethod method, Json params, std::optional<Json> id) {
    if (!params.is_object())
        throw std::invalid_argument("MCP App message params must be an object");
    Json message = {
        {"jsonrpc", "2.0"}, {"method", to_string(method)}, {"params", std::move(params)}};
    if (id)
        message["id"] = std::move(*id);
    return message;
}
void validate_ui_message(UiMethod method, const Json &message, bool notification) {
    if (!message.is_object() || message.value("jsonrpc", Json()) != "2.0" ||
        !message.contains("method") || !message["method"].is_string() ||
        text(message["method"]) != to_string(method) || !message.contains("params") ||
        !message["params"].is_object())
        invalid("Malformed MCP Apps bridge message");
    const bool method_is_notification =
        method == UiMethod::initialized || method == UiMethod::request_teardown ||
        method == UiMethod::tool_input || method == UiMethod::tool_input_partial ||
        method == UiMethod::tool_result || method == UiMethod::tool_cancelled ||
        method == UiMethod::size_changed || method == UiMethod::host_context_changed ||
        method == UiMethod::sandbox_proxy_ready || method == UiMethod::sandbox_resource_ready;
    if (notification != method_is_notification ||
        (notification ? message.contains("id")
                      : (!message.contains("id") || !valid_id(message["id"]))))
        invalid("MCP Apps bridge message has invalid request/notification ID shape");
    const auto &p = message["params"];
    auto str = [&](std::string_view key) { return p.contains(key) && p[key].is_string(); };
    switch (method) {
    case UiMethod::open_link:
        if (!str("url"))
            invalid("ui/open-link requires url");
        break;
    case UiMethod::message:
        if (!p.contains("role") || p["role"] != "user" || !p.contains("content") ||
            !p["content"].is_array())
            invalid("ui/message requires user role and content");
        break;
    case UiMethod::initialize:
        if (!p.contains("appInfo") || !p["appInfo"].is_object() || !p.contains("appCapabilities") ||
            !p["appCapabilities"].is_object() || !str("protocolVersion"))
            invalid("ui/initialize fields are invalid");
        break;
    case UiMethod::request_display_mode:
        if (!p.contains("mode") ||
            (p["mode"] != "inline" && p["mode"] != "fullscreen" && p["mode"] != "pip"))
            invalid("Invalid display mode");
        break;
    case UiMethod::resource_teardown:
    case UiMethod::initialized:
    case UiMethod::request_teardown:
    case UiMethod::sandbox_proxy_ready:
        break;
    case UiMethod::sandbox_resource_ready:
        if (!str("html"))
            invalid("sandbox-resource-ready requires html");
        break;
    case UiMethod::tool_input:
    case UiMethod::tool_input_partial:
        if (p.contains("arguments") && !p["arguments"].is_object())
            invalid("Tool input arguments must be an object");
        break;
    case UiMethod::tool_result:
        if (!p.contains("content") || !p["content"].is_array())
            invalid("Tool result requires content");
        break;
    case UiMethod::tool_cancelled:
        if (p.contains("reason") && !p["reason"].is_string())
            invalid("reason must be a string");
        break;
    case UiMethod::size_changed:
        for (auto key : {"width", "height"})
            if (p.contains(key) && !p[key].is_number())
                invalid("size dimensions must be numeric");
        break;
    case UiMethod::host_context_changed:
        break;
    case UiMethod::update_model_context:
        if ((p.contains("content") && !p["content"].is_array()) ||
            (p.contains("structuredContent") && !p["structuredContent"].is_object()))
            invalid("Invalid model context update");
        break;
    }
}
Json draft_ui_message(DraftUiMethod method, Json params, std::optional<Json> id) {
    if (!params.is_object())
        throw std::invalid_argument("Draft MCP Apps message params must be an object");
    Json message = {
        {"jsonrpc", "2.0"}, {"method", to_string(method)}, {"params", std::move(params)}};
    if (id)
        message["id"] = std::move(*id);
    return message;
}
void validate_draft_ui_message(DraftUiMethod method, const Json &message, bool notification) {
    if (method != DraftUiMethod::download_file || !message.is_object() ||
        message.value("jsonrpc", Json()) != "2.0" || !message.contains("method") ||
        !message["method"].is_string() || text(message["method"]) != to_string(method) ||
        !message.contains("params") || !message["params"].is_object() ||
        !message["params"].contains("contents") || !message["params"]["contents"].is_array())
        invalid("Malformed draft MCP Apps download-file request");
    if (notification || !message.contains("id") || !valid_id(message["id"]))
        invalid("ui/download-file must be a JSON-RPC request");
    for (const auto &content : message["params"]["contents"])
        if (!content.is_object() || (!content.contains("resource") && !content.contains("uri")))
            invalid("download-file contents must be embedded resources or resource links");
}

} // namespace mcp::extensions
