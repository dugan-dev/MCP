#pragma once

#include <mcp/transport.h>

namespace mcp {

namespace extensions {
class SkillsClient;
}

struct ClientOptions {
  Json info = {{"name", "MCP"}, {"version", library_version}};
  Json capabilities = Json::object();
  // JSON allocations retain this optional thread-safe resource.
  JsonStorage memory_resource;
  std::size_t max_pages = 10000;
  std::size_t max_input_rounds = 16;
  std::size_t max_list_items = 100000;
  std::size_t max_list_bytes = 64 * 1024 * 1024;
  std::size_t max_input_requests = 128;
};

using InputHandler = std::function<Json(const Json&)>;

class Client final {
 public:
  explicit Client(std::shared_ptr<Transport>, ClientOptions = {});
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Requests and server discovery.
  Json request(std::string_view method, Json params = Json::object(), RequestOptions options = {}, NotificationHandler = {});
  Json discover(RequestOptions = {});

  // Tools, resources and prompts.
  Json list_tools(RequestOptions = {});
  Json call_tool(std::string_view name, Json arguments = Json::object(), RequestOptions = {}, NotificationHandler = {});
  Json list_resources(RequestOptions = {});
  Json list_resource_templates(RequestOptions = {});
  Json read_resource(std::string_view uri, RequestOptions = {});
  Json list_prompts(RequestOptions = {});
  Json get_prompt(std::string_view name, Json arguments = Json::object(), RequestOptions = {});
  Json complete(Json reference, Json argument, RequestOptions = {});

  // Notifications and input workflows.
  Json listen(Json filters, RequestOptions, NotificationHandler);
  Json request_with_input(std::string_view method, Json params, InputHandler, RequestOptions = {}, NotificationHandler = {});

  void close() noexcept;

 private:
  friend class extensions::SkillsClient;
  Json list(std::string_view method, std::string_view field, RequestOptions);
  Json list_result(std::string_view method, std::string_view field, RequestOptions,
                   std::function<void(const Json& page, const Json& first_page)> validate_page = {});
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mcp
