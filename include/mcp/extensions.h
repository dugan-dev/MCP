#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <mcp/client.h>
#include <mcp/server.h>

namespace mcp::extensions {

inline constexpr std::string_view tasks_id = "io.modelcontextprotocol/tasks";
inline constexpr std::string_view skills_id = "io.modelcontextprotocol/skills";
inline constexpr std::string_view apps_id = "io.modelcontextprotocol/ui";
inline constexpr std::string_view apps_mime_type = "text/html;profile=mcp-app";

enum class TaskStatus { working, input_required, completed, cancelled, failed };
std::string_view to_string(TaskStatus) noexcept;
TaskStatus task_status(std::string_view);

// `wire` contains the exact Task fields returned by the stable Tasks extension.
// owner and revision are storage metadata and are never serialized.
struct TaskRecord {
    std::string owner;
    std::uint64_t revision = 0;
    Json wire = Json::object();
    std::set<std::string, std::less<>> answered_input_keys;
    bool cancel_requested = false;
    // Monotonic expiry used only by MemoryTaskStore; never serialized.
    std::optional<std::chrono::steady_clock::time_point> memory_created;
    std::optional<std::chrono::steady_clock::time_point> memory_expiry;
    // Appended to preserve the meaning of existing aggregate initializers.
    // Accepted answers for the host's task runner; never serialized on the wire.
    Json input_responses = Json::object();
};

void validate_task(const Json &, bool create_result = false);
Json task_result(const TaskRecord &);
Json create_task_result(const TaskRecord &);

// Durable implementations must make compare_exchange atomic and compare the
// current revision. A successful write stores `replacement` at expected+1.
class TaskStore {
  public:
    virtual ~TaskStore() = default;
    virtual TaskRecord create(TaskRecord) = 0;
    virtual std::optional<TaskRecord> get(std::string_view owner, std::string_view task_id) = 0;
    virtual bool compare_exchange(std::string_view owner, std::string_view task_id,
                                  std::uint64_t expected_revision, TaskRecord replacement) = 0;
    virtual bool erase(std::string_view owner, std::string_view task_id,
                       std::uint64_t expected_revision) = 0;
};

class MemoryTaskStore final : public TaskStore {
  public:
    explicit MemoryTaskStore(std::size_t max_tasks = 100000);
    TaskRecord create(TaskRecord) override;
    std::optional<TaskRecord> get(std::string_view owner, std::string_view task_id) override;
    bool compare_exchange(std::string_view owner, std::string_view task_id,
                          std::uint64_t expected_revision, TaskRecord replacement) override;
    bool erase(std::string_view owner, std::string_view task_id,
               std::uint64_t expected_revision) override;
    // Also performed by every store operation. Idle hosts may call this from
    // their own maintenance loop; the store does not launch background threads.
    std::size_t prune_expired();

  private:
    using TaskKey = std::pair<std::string, std::string>;
    using ExpiryIndex = std::multimap<std::chrono::steady_clock::time_point, TaskKey>;
    struct StoredTask {
        TaskRecord record;
        std::optional<ExpiryIndex::iterator> expiry;
    };
    std::size_t prune_expired_locked(std::chrono::steady_clock::time_point);
    std::mutex mutex_;
    ExpiryIndex expirations_;
    std::map<TaskKey, StoredTask> tasks_;
    std::size_t max_tasks_;
};

struct TaskServerOptions {
    // Must map trusted dispatch attributes to a stable tenant/principal key.
    // If unset, owner identity is the stable `iss` + `sub` pair in trusted
    // attributes. Missing stable identity fails closed; callers may supply a
    // resolver for another authenticated principal model.
    std::function<std::string(const Json &trusted_attributes)> owner;
    std::function<void(std::string_view owner, std::string_view task_id)> on_cancel;
    std::chrono::milliseconds default_ttl{24 * 60 * 60 * 1000};
    std::chrono::milliseconds poll_interval{1000};
};

void install_tasks(Server &, std::shared_ptr<TaskStore>, TaskServerOptions = {});
Json tasks_capability();
TaskRecord make_task(std::string owner, std::string task_id, TaskStatus,
                     std::chrono::milliseconds ttl, std::chrono::milliseconds poll_interval,
                     std::string status_message = {});
TaskRecord create_task(RequestContext &, TaskStore &, const TaskServerOptions &,
                       std::chrono::milliseconds ttl, std::chrono::milliseconds poll_interval,
                       std::string status_message = {});
TaskRecord create_task(RequestContext &, TaskStore &, const TaskServerOptions &,
                       std::string status_message = {});
void transition_task(TaskRecord &, TaskStatus, Json payload = Json::object());
void request_task_cancel(TaskRecord &);
void accept_task_inputs(TaskRecord &, const Json &input_responses);

// Borrows Client, which must outlive this wrapper and every wrapper call.
class TaskClient final {
  public:
    explicit TaskClient(Client &client) : client_(client) {}
    Json call_tool(std::string_view name, Json arguments = Json::object(), RequestOptions = {},
                   NotificationHandler = {});
    Json run_tool(std::string_view name, Json arguments = Json::object(), InputHandler = {},
                  RequestOptions = {}, NotificationHandler = {});
    Json get(std::string_view task_id, RequestOptions = {});
    Json update(std::string_view task_id, Json input_responses, RequestOptions = {});
    Json cancel(std::string_view task_id, RequestOptions = {});
    Json run(std::string_view task_id, InputHandler = {}, RequestOptions = {},
             NotificationHandler = {});

  private:
    Client &client_;
};

struct SkillFile {
    std::string uri;
    std::string relative_path;
    std::string mime_type;
    std::string digest;
    std::size_t size = 0;
};

struct SkillListOptions {
    std::size_t page_size = 100;
    std::uint64_t ttl_ms = 300000;
    std::string cache_scope = "private";
};

// A filesystem-backed stable Skills provider. It never follows symlinks and
// resolves every read beneath the canonical root. Frontmatter is retained as
// caller-supplied JSON, or minimally extracts the required name/description.
class SkillDirectory final {
  public:
    using FrontmatterParser = std::function<Json(std::string_view yaml_frontmatter)>;
    SkillDirectory(std::filesystem::path root, std::string uri_prefix = "skill://",
                   FrontmatterParser = {}, SkillListOptions = {});
    Json list(std::string_view cursor = {}) const;
    Json get(std::string_view skill_uri) const;
    Json read(std::string_view resource_uri) const;
    Json read_directory(std::string_view directory_uri) const;
    Json resource_descriptors() const;

  private:
    std::filesystem::path root_;
    std::string prefix_;
    FrontmatterParser frontmatter_parser_;
    SkillListOptions list_options_;
    std::filesystem::path safe_path(std::string_view relative) const;
    Json entry(const std::filesystem::path &skill_dir) const;
};
void install_skills(Server &, std::shared_ptr<SkillDirectory>, bool directory_read = false);
// Borrows Client, which must outlive this wrapper and every wrapper call.
class SkillsClient final {
  public:
    explicit SkillsClient(Client &client) : client_(client) {}
    Json list(RequestOptions = {});
    Json get(std::string_view skill_md_uri, RequestOptions = {});
    Json read_directory(std::string_view uri, RequestOptions = {});

  private:
    Client &client_;
};

Json skills_capability(bool directory_read = false);

enum class UiMethod {
    open_link,
    message,
    initialize,
    initialized,
    update_model_context,
    request_display_mode,
    resource_teardown,
    request_teardown,
    tool_input,
    tool_input_partial,
    tool_result,
    tool_cancelled,
    size_changed,
    host_context_changed,
    sandbox_proxy_ready,
    sandbox_resource_ready
};
// Present in the pinned Apps SDK and draft specification only; kept separate
// so stable 2026-01-26 validation cannot accept it by accident.
enum class DraftUiMethod { download_file };
std::string_view to_string(UiMethod) noexcept;
std::string_view to_string(DraftUiMethod) noexcept;
Json ui_capability();
Json ui_tool_metadata(std::string_view resource_uri,
                      std::vector<std::string> visibility = {"model", "app"});
Resource ui_resource(std::string uri, std::string name, Json ui_metadata = Json::object(),
                     std::string description = {});
Json ui_resource_contents(std::string_view uri, std::string html,
                          Json ui_metadata = Json::object());
Json ui_message(UiMethod, Json params = Json::object(), std::optional<Json> id = {});
void validate_ui_message(UiMethod, const Json &message, bool notification);
Json draft_ui_message(DraftUiMethod, Json params = Json::object(), std::optional<Json> id = {});
void validate_draft_ui_message(DraftUiMethod, const Json &message, bool notification);

} // namespace mcp::extensions
