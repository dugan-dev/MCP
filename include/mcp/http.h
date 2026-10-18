#pragma once
#include <map>
#include <mcp/server.h>
#include <mcp/auth.h>
#include <mcp/transport.h>
#include <vector>
namespace mcp {
using Headers = std::map<std::string, std::string, std::less<>>;
struct HttpClientOptions {
    std::string url;
    Headers headers;
    std::function<std::string(std::string_view, RequestOptions)> bearer_token;
    std::string ca_file;
    std::size_t connections = 16;
    Limits limits;
    // JSON allocations retain this optional thread-safe resource.
    JsonStorage memory_resource;
};
class HttpTransport final : public Transport {
  public:
    explicit HttpTransport(HttpClientOptions);
    ~HttpTransport() override;
    Json exchange(const Json &, const RequestOptions &, NotificationHandler = {}) override;
    void set_tool_schema(std::string_view, const Json &) override;
    void set_tool_schemas(const Json &) override;
    void close() noexcept override;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
struct HttpServerOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string path = "/mcp";
    std::size_t io_threads = 2;
    std::size_t worker_threads = 4;
    std::size_t max_connections = 4096;
    std::chrono::milliseconds idle_timeout{30000};
    std::vector<std::string> allowed_origins;
    std::vector<std::string> allowed_hosts;
    std::shared_ptr<ResourceAuthorization> resource_authorization;
    std::function<bool(const Headers &)> authorize;
    std::function<std::optional<Json>(const Headers &)> authenticate;
    std::string certificate_file;
    std::string private_key_file;
};
class HttpServer final {
  public:
    // The borrowed Server must outlive this listener, including stop/destruction.
    explicit HttpServer(Server &, HttpServerOptions = {});
    ~HttpServer();
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;
    void start();
    void stop() noexcept;
    std::uint16_t port() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace mcp
