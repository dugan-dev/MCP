#include "deadline.h"
#include "containers.h"
#include "http_headers.h"
#include "notification.h"
#include <atomic>
#include <charconv>
#include <climits>
#include <condition_variable>
#include <curl/curl.h>
#include <mcp/http.h>
#include <mcp/schema.h>
#include <mcp/sse.h>
#include <mcp/memory.h>
#include <mutex>

namespace mcp {
namespace {
struct CurlRuntime {
  CurlRuntime() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw TransportError(TransportErrorKind::connection, "curl initialization failed");
  }
  ~CurlRuntime() { curl_global_cleanup(); }
};
CurlRuntime& runtime() {
  static CurlRuntime value;
  return value;
}
struct Slot {
  CURL* easy = nullptr;
  CURLM* multi = nullptr;
  bool busy = false;
  Slot() : easy(curl_easy_init()), multi(curl_multi_init()) {
    if (!easy || !multi) {
      if (easy)
        curl_easy_cleanup(easy);
      if (multi)
        curl_multi_cleanup(multi);
      throw std::bad_alloc();
    }
  }
  ~Slot() {
    curl_multi_cleanup(multi);
    curl_easy_cleanup(easy);
  }
};
struct Exchange {
  detail::String body;
  long status = 0;
  std::string content_type;
  Headers response_headers;
  const Json& request;
  const Limits& limits;
  NotificationHandler notify;
  bool capture_authorization_challenges = false;
  std::exception_ptr error;
  std::optional<Json> response;
  SseDecoder decoder;
  std::size_t total_bytes = 0, header_bytes = 0;
  bool streaming = false;
  detail::NotificationState notification_state;
  Exchange(std::pmr::memory_resource* memory, const Json& r, const Limits& l, NotificationHandler n)
      : body(memory),
        request(r),
        limits(l),
        notify(std::move(n)),
        decoder([this](std::string_view bytes) { message(parse_json(bytes, limits.message_bytes, limits.json_depth)); }, l.message_bytes) {}
  void accept_response(Json value) {
    detail::validate_stream_end(value, request, notification_state);
    if ((status < 200 || status >= 300) && !value.contains("error"))
      throw TransportError(TransportErrorKind::invalid_response, "HTTP error without JSON-RPC error");
    if (response)
      throw TransportError(TransportErrorKind::invalid_response, "Multiple responses for one request");
    response = std::move(value);
  }
  void message(Json value) {
    if (value.is_object() && value.contains("method")) {
      // The final HTTP status is known before body delivery. Error bodies
      // may contain a matching RPC error, but must never invoke callbacks.
      if (status < 200 || status >= 300)
        return;
      detail::validate_notification(value, request, notification_state);
      if (notify)
        notify(value);
    } else
      accept_response(std::move(value));
  }
};
std::string trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return std::string(value);
}
std::string_view media_type(std::string_view value) noexcept {
  value = value.substr(0, value.find(';'));
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return value;
}
std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user) noexcept {
  auto& x = *static_cast<Exchange*>(user);
  const auto bytes = size * count;
  try {
    if (bytes > 65536 - x.header_bytes)
      throw TransportError(TransportErrorKind::limit, "HTTP headers exceed byte limit");
    x.header_bytes += bytes;
    std::string_view line(data, bytes);
    if (line.starts_with("HTTP/")) {
      x.content_type.clear();
      x.response_headers.clear();
      x.streaming = false;
      const auto space = line.find(' ');
      if (space == std::string_view::npos || std::from_chars(line.data() + space + 1, line.data() + line.size(), x.status).ec != std::errc())
        throw TransportError(TransportErrorKind::invalid_response, "Invalid HTTP status");
    } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
      const auto key = detail::lower(line.substr(0, colon));
      x.response_headers[key] = trim(line.substr(colon + 1));
      if (key == "content-type") {
        x.content_type = detail::lower(trim(line.substr(colon + 1)));
        x.streaming = media_type(x.content_type) == "text/event-stream";
      }
    }
    return bytes;
  } catch (...) {
    x.error = std::current_exception();
    return 0;
  }
}
std::size_t on_data(char* data, std::size_t size, std::size_t count, void* user) noexcept {
  auto& x = *static_cast<Exchange*>(user);
  const auto bytes = size * count;
  try {
    if (x.capture_authorization_challenges && (x.status == 401 || x.status == 403)) {
      if (bytes > x.limits.message_bytes - x.body.size())
        throw TransportError(TransportErrorKind::limit, "HTTP body exceeds byte limit");
      x.body.append(data, bytes);
      return bytes;
    }
    if (!x.streaming && media_type(x.content_type) != "application/json")
      throw TransportError(TransportErrorKind::invalid_response, "Expected JSON or SSE content type");
    if (x.streaming)
      x.decoder.feed(std::string_view(data, bytes));
    else {
      if (bytes > x.limits.message_bytes - x.body.size())
        throw TransportError(TransportErrorKind::limit, "HTTP body exceeds byte limit");
      x.body.append(data, bytes);
    }
    x.total_bytes += bytes;
    return bytes;
  } catch (const RpcError&) {
    x.error = std::make_exception_ptr(TransportError(TransportErrorKind::invalid_response, "Invalid SSE JSON"));
    return 0;
  } catch (...) {
    x.error = std::current_exception();
    return 0;
  }
}
}  // namespace
struct HttpTransport::Impl {
  HttpClientOptions options;
  std::mutex mutex;
  std::condition_variable_any available;
  std::atomic<bool> closed{false};
  std::vector<std::unique_ptr<Slot>> slots;
  std::map<std::string, Json, std::less<>> schemas;
  explicit Impl(HttpClientOptions config) : options(std::move(config)) {
    if (!options.memory_resource)
      options.memory_resource = memory::default_resource();
    (void)runtime();
    if ((!options.url.starts_with("http://") && !options.url.starts_with("https://")) || !options.connections || !options.limits.message_bytes)
      throw std::invalid_argument("HTTP(S) URL, connections and byte limit are required");
    for (const auto& [key, value] : options.headers)
      if (!detail::header_token(key) || value.find_first_of("\r\n\0", 0, 3) != std::string::npos || detail::lower(key).starts_with("mcp-") ||
          detail::lower(key) == "content-type" || detail::lower(key) == "accept" || detail::lower(key) == "content-length" ||
          detail::lower(key) == "transfer-encoding")
        throw std::invalid_argument("Unsafe or reserved HTTP header");
    for (std::size_t i = 0; i < options.connections; ++i)
      slots.push_back(std::make_unique<Slot>());
  }
};
HttpTransport::HttpTransport(HttpClientOptions options) : impl_(std::make_shared<Impl>(std::move(options))) {}
HttpTransport::~HttpTransport() {
  close();
}
void HttpTransport::close() noexcept {
  impl_->closed.store(true);
  impl_->available.notify_all();
}
void HttpTransport::set_tool_schema(std::string_view name, const Json& schema) {
  JsonScope scope(impl_->options.memory_resource);
  detail::validate_header_schema(schema);
  (void)Schema(schema);
  std::lock_guard lock(impl_->mutex);
  impl_->schemas.insert_or_assign(std::string(name), schema);
}
void HttpTransport::set_tool_schemas(const Json& tools) {
  JsonScope scope(impl_->options.memory_resource);
  std::map<std::string, Json, std::less<>> schemas;
  for (const auto& tool : tools) {
    const auto& schema = tool["inputSchema"];
    (void)Schema(schema);
    detail::validate_header_schema(schema);
    schemas.emplace(std::string(text(tool["name"])), schema);
  }
  std::lock_guard lock(impl_->mutex);
  impl_->schemas.swap(schemas);
}
Json HttpTransport::exchange(const Json& request, const RequestOptions& options, NotificationHandler notify) {
  auto impl = impl_;
  JsonScope scope(impl->options.memory_resource);
  validate_request(request);
  if (options.timeout.count() <= 0 || options.timeout.count() > LONG_MAX)
    throw std::invalid_argument("Invalid HTTP timeout");
  const auto deadline = detail::request_deadline(options.timeout);
  Slot* slot = nullptr;
  Json schema;
  {
    std::unique_lock lock(impl->mutex);
    const bool ready = impl->available.wait_until(lock, options.stop, deadline, [&] {
      if (impl->closed.load())
        return true;
      for (auto& candidate : impl->slots)
        if (!candidate->busy) {
          slot = candidate.get();
          return true;
        }
      return false;
    });
    if (options.stop.stop_requested())
      throw TransportError(TransportErrorKind::cancelled, "Request cancelled");
    if (impl->closed.load())
      throw TransportError(TransportErrorKind::closed, "HTTP transport is closed");
    if (!ready || !slot)
      throw TransportError(TransportErrorKind::timeout, "HTTP pool deadline exceeded");
    slot->busy = true;
    if (request["method"] == "tools/call" && request["params"].contains("name") && request["params"]["name"].is_string()) {
      const auto it = impl->schemas.find(text(request["params"]["name"]));
      if (it != impl->schemas.end())
        schema = it->second;
    }
  }
  struct Lease {
    std::shared_ptr<Impl> impl;
    Slot* slot;
    ~Lease() {
      curl_multi_remove_handle(slot->multi, slot->easy);
      std::lock_guard lock(impl->mutex);
      slot->busy = false;
      impl->available.notify_one();
    }
  } lease{impl, slot};
  curl_easy_reset(slot->easy);
  std::string body = serialize_json(request);
  if (body.size() > impl->options.limits.message_bytes)
    throw TransportError(TransportErrorKind::limit, "Request exceeds byte limit");
  Headers headers = detail::request_headers(request, schema);
  headers["accept"] = "application/json, text/event-stream";
  headers["content-type"] = "application/json";
  for (const auto& [key, value] : impl->options.headers)
    headers[detail::lower(key)] = value;
  if (impl->options.bearer_token) {
    if (headers.contains("authorization"))
      throw std::invalid_argument("Choose either bearer_token or Authorization header");
    const auto token = impl->options.bearer_token(impl->options.url, options);
    if (token.empty() || token.find_first_of(" \r\n\t\0", 0, 5) != std::string::npos)
      throw AuthError("invalid_token", "OAuth provider returned an invalid token");
    headers["authorization"] = "Bearer " + token;
  }
  curl_slist* raw = nullptr;
  for (const auto& [key, value] : headers) {
    auto* next = curl_slist_append(raw, (key + ": " + value).c_str());
    if (!next) {
      curl_slist_free_all(raw);
      throw std::bad_alloc();
    }
    raw = next;
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list(raw, curl_slist_free_all);
  Exchange x(impl->options.memory_resource.get(), request, impl->options.limits, std::move(notify));
  x.capture_authorization_challenges = impl->options.capture_authorization_challenges;
  auto set = [&](CURLoption option, auto value) {
    if (curl_easy_setopt(slot->easy, option, value) != CURLE_OK)
      throw TransportError(TransportErrorKind::connection, "Cannot configure curl");
  };
  set(CURLOPT_URL, impl->options.url.c_str());
  set(CURLOPT_HTTPHEADER, raw);
  set(CURLOPT_POST, 1L);
  set(CURLOPT_POSTFIELDS, body.data());
  set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  set(CURLOPT_PROTOCOLS_STR, "http,https");
  set(CURLOPT_FOLLOWLOCATION, 0L);
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_SSL_VERIFYPEER, 1L);
  set(CURLOPT_SSL_VERIFYHOST, 2L);
  if (!impl->options.ca_file.empty())
    set(CURLOPT_CAINFO, impl->options.ca_file.c_str());
  set(CURLOPT_TIMEOUT_MS, static_cast<long>(options.timeout.count()));
  set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.timeout.count()));
  set(CURLOPT_WRITEFUNCTION, on_data);
  set(CURLOPT_WRITEDATA, &x);
  set(CURLOPT_HEADERFUNCTION, on_header);
  set(CURLOPT_HEADERDATA, &x);
  set(CURLOPT_TCP_NODELAY, 1L);
  set(CURLOPT_ACCEPT_ENCODING, "");
  if (curl_multi_add_handle(slot->multi, slot->easy) != CURLM_OK)
    throw TransportError(TransportErrorKind::connection, "Cannot start HTTP exchange");
  int running = 0;
  do {
    if (options.stop.stop_requested())
      throw TransportError(TransportErrorKind::cancelled, "Request cancelled");
    if (impl->closed.load())
      throw TransportError(TransportErrorKind::closed, "HTTP transport is closed");
    if (std::chrono::steady_clock::now() >= deadline)
      throw TransportError(TransportErrorKind::timeout, "HTTP deadline exceeded");
    const auto code = curl_multi_perform(slot->multi, &running);
    if (code != CURLM_OK)
      throw TransportError(TransportErrorKind::connection, curl_multi_strerror(code));
    if (x.error)
      std::rethrow_exception(x.error);
    if (x.response)
      break;
    if (running) {
      int ready = 0;
      if (curl_multi_poll(slot->multi, nullptr, 0, 100, &ready) != CURLM_OK)
        throw TransportError(TransportErrorKind::connection, "HTTP poll failed");
    }
  } while (running);
  if (!x.response) {
    int count = 0;
    CURLMsg* message = nullptr;
    while ((message = curl_multi_info_read(slot->multi, &count)))
      if (message->msg == CURLMSG_DONE && message->data.result != CURLE_OK) {
        if (message->data.result == CURLE_OPERATION_TIMEDOUT)
          throw TransportError(TransportErrorKind::timeout, "HTTP deadline exceeded");
        throw TransportError(TransportErrorKind::connection, curl_easy_strerror(message->data.result));
      }
    curl_easy_getinfo(slot->easy, CURLINFO_RESPONSE_CODE, &x.status);
    if (x.capture_authorization_challenges && (x.status == 401 || x.status == 403))
      throw HttpAuthorizationError(static_cast<unsigned>(x.status), std::move(x.response_headers));
    if (x.streaming) {
      x.decoder.finish();
      throw TransportError(TransportErrorKind::invalid_response, "SSE stream ended without a response");
    }
    if (media_type(x.content_type) != "application/json")
      throw TransportError(TransportErrorKind::invalid_response, "Expected JSON or SSE content type");
    try {
      x.accept_response(parse_json(x.body, impl->options.limits.message_bytes, impl->options.limits.json_depth));
    } catch (const RpcError&) {
      throw TransportError(TransportErrorKind::invalid_response, "Invalid HTTP JSON");
    }
    if (!x.response)
      throw TransportError(TransportErrorKind::invalid_response, "HTTP response contains no result");
  }
  return std::move(*x.response);
}
}  // namespace mcp
