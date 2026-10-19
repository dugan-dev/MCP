#pragma once

#include <mcp/json.h>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <stop_token>

namespace mcp {

inline constexpr std::string_view protocol_version = "2026-07-28";
inline constexpr std::string_view library_version = "1.0.0";
inline constexpr std::string_view version_key = "io.modelcontextprotocol/protocolVersion";
inline constexpr std::string_view capabilities_key = "io.modelcontextprotocol/clientCapabilities";
inline constexpr std::string_view client_info_key = "io.modelcontextprotocol/clientInfo";
inline constexpr std::string_view server_info_key = "io.modelcontextprotocol/serverInfo";
inline constexpr std::string_view subscription_key = "io.modelcontextprotocol/subscriptionId";
enum class ErrorCode : int {
  parse_error = -32700,
  invalid_request = -32600,
  method_not_found = -32601,
  invalid_params = -32602,
  internal_error = -32603,
  header_mismatch = -32020,
  missing_capability = -32021,
  unsupported_version = -32022
};

class RpcError : public std::runtime_error {
 public:
  RpcError(ErrorCode code, std::string message, Json data = nullptr);
  RpcError(int code, std::string message, Json data = nullptr);
  int code() const noexcept { return code_; }
  const Json& data() const noexcept { return data_; }

 private:
  int code_;
  Json data_;
};

enum class TransportErrorKind { connection, timeout, cancelled, limit, invalid_response, closed };
class TransportError : public std::runtime_error {
 public:
  TransportError(TransportErrorKind kind, std::string message) : std::runtime_error(std::move(message)), kind_(kind) {}
  TransportErrorKind kind() const noexcept { return kind_; }

 private:
  TransportErrorKind kind_;
};

struct Limits {
  std::size_t message_bytes = 8 * 1024 * 1024;
  std::size_t json_depth = 128;
  std::size_t page_size = 100;
  std::size_t pending_requests = 1024;
  std::size_t stream_queue_bytes = 8 * 1024 * 1024;
};

struct RequestOptions {
  std::chrono::milliseconds timeout{30000};
  std::stop_token stop;
  Json meta = Json::object();
};

using NotificationHandler = std::function<void(const Json&)>;
Json make_request(Json id, std::string_view method, Json params = Json::object(), Json capabilities = Json::object(),
                  Json info = {{"name", "MCP"}, {"version", library_version}});
Json make_notification(std::string_view method, Json params = Json::object());
Json make_error(const Json& id, int code, std::string_view message, Json data = nullptr);
bool valid_id(const Json&) noexcept;
void validate_request(const Json&);
void validate_response(const Json&, const Json& id);
Json text_result(std::string_view value, bool is_error = false);
Json input_required(Json requests, std::string state = {});

}  // namespace mcp
