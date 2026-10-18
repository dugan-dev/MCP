#pragma once
#include <mcp/prompt.h>
#include <mcp/resource.h>
#include <mcp/tool.h>
#include <memory>
#include <mutex>
#include <optional>
namespace mcp {
// Borrowed dispatch state: do not retain this context or its Json references
// after the handler returns. Copy Json values needed by later work.
class RequestContext final {
  public:
    const Json &id() const noexcept;
    const Json &params() const noexcept;
    const Json &capabilities() const noexcept;
    const Json &attributes() const noexcept;
    std::stop_token stop_token() const noexcept;
    void throw_if_cancelled() const;
    void progress(double value, std::optional<double> total = {}, std::string_view message = {});
    void require_capability(std::string_view name) const;
    Json elicit(std::string_view key, Json params, std::string state = {}) const;
    Json request_input(std::string_view key, Json request, std::string state = {}) const;
    void log(std::string_view level, Json data, std::string_view logger = {}) const;

  private:
    friend class Server;
    const Json *request_ = nullptr;
    const Json *attributes_ = nullptr;
    std::stop_token stop_;
    NotificationHandler notify_;
    std::optional<double> last_progress_;
};
// Worker threads may invoke the same registered handler concurrently.
using Handler = std::function<Json(const Json &, RequestContext &)>;
// Resolve a trusted principal's access to a task, or throw when unavailable.
// The returned owner key is host metadata and is never taken from RPC params.
using TaskSubscriptionAuthorizer =
    std::function<std::string(const Json &trusted_attributes, std::string_view task_id)>;
struct DispatchOptions {
    std::stop_token stop;
    NotificationHandler notify;
    Json attributes;
};
struct ServerOptions {
    Json info = {{"name", "MCP"}, {"version", "0.0.1"}};
    std::string instructions;
    Limits limits;
    // JSON allocations retain this optional thread-safe resource.
    JsonStorage memory_resource;
    std::uint64_t cache_ttl_ms = 0;
    std::string cache_scope = "private";
    std::size_t max_subscriptions = 1024;
};
class Subscription final {
  public:
    ~Subscription();
    void close();

  private:
    friend class Server;
    explicit Subscription(std::function<void()> close) : close_(std::move(close)) {}
    std::function<void()> close_;
    std::mutex mutex_;
};
class Server final {
  public:
    explicit Server(ServerOptions options = {});
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    void add_tool(Tool, Handler);
    bool remove_tool(std::string_view name);
    void add_resource(Resource, Handler);
    bool remove_resource(std::string_view uri);
    void add_resource_template(ResourceTemplate);
    void set_resource_resolver(Handler);
    void add_prompt(Prompt, Handler);
    bool remove_prompt(std::string_view name);
    void set_completion_handler(Handler);
    void add_extension(std::string identifier, Json settings, std::string method, Handler);
    Json capabilities() const;
    Json tool_schema(std::string_view name) const;
    Json dispatch(const Json &, std::stop_token = {}, NotificationHandler = {}) const;
    Json dispatch(const Json &, DispatchOptions) const;
    Json dispatch(std::string_view, std::stop_token = {}, NotificationHandler = {}) const;
    std::shared_ptr<Subscription> subscribe(const Json &, NotificationHandler,
                                            Json trusted_attributes = Json::object());
    void set_task_subscription_authorizer(TaskSubscriptionAuthorizer);
    void resource_updated(std::string_view uri);
    // Task publishers must pass the owning TaskRecord's trusted owner key.
    void notify(std::string_view method, Json params, std::string_view task_owner = {});
    const ServerOptions &options() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace mcp
