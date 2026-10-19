#include <mcp/http.h>
#include "deadline.h"

#include <atomic>
#include <algorithm>
#include <set>
#include <sstream>

namespace mcp {
namespace {
thread_local std::vector<const void*> authorizing;
struct AuthorizationScope {
  explicit AuthorizationScope(const void* transport) { authorizing.push_back(transport); }
  ~AuthorizationScope() { authorizing.pop_back(); }
};
std::string challenge_field(std::string_view value, std::string_view name) {
  // Parse comma-separated auth parameters; do not mistake a substring in a quoted value for a parameter.
  auto at = value.find(' ');
  if (at == std::string_view::npos)
    return {};
  while (at < value.size()) {
    while (at < value.size() && (value[at] == ' ' || value[at] == ',' || value[at] == '\t'))
      ++at;
    auto end = value.find('=', at);
    if (end == std::string_view::npos)
      break;
    auto key = value.substr(at, end - at);
    while (!key.empty() && key.back() == ' ')
      key.remove_suffix(1);
    at = end + 1;
    while (at < value.size() && value[at] == ' ')
      ++at;
    std::string decoded;
    if (at < value.size() && value[at] == '"') {
      ++at;
      while (at < value.size() && value[at] != '"') {
        if (value[at] == '\\' && at + 1 < value.size())
          ++at;
        decoded += value[at++];
      }
      if (at < value.size())
        ++at;
    } else {
      const auto stop = value.find(',', at);
      decoded = value.substr(at, stop == std::string_view::npos ? value.size() - at : stop - at);
      at = stop == std::string_view::npos ? value.size() : stop;
    }
    if (key == name)
      return decoded;
  }
  return {};
}
std::string scopes_union(std::string_view first, std::string_view second) {
  std::set<std::string> values;
  for (auto text : {first, second}) {
    std::istringstream stream{std::string(text)};
    for (std::string value; stream >> value;)
      values.insert(std::move(value));
  }
  std::string result;
  for (const auto& value : values) {
    if (!result.empty())
      result += ' ';
    result += value;
  }
  return result;
}
std::string json_text(const Json& value, const char* key) {
  return value.contains(key) && value[key].is_string() ? std::string(text(value[key])) : std::string();
}
}  // namespace
struct OAuthTransport::Impl {
  OAuthTransportOptions options;
  std::mutex gate;
  std::atomic<bool> closed{false};
  std::atomic<std::shared_ptr<HttpTransport>> active;
  std::map<std::string, Json, std::less<>> schemas;
  std::string granted_scopes;
  explicit Impl(OAuthTransportOptions value) : options(std::move(value)) {
    if (!options.max_authorization_attempts || options.max_authorization_attempts > 16)
      throw std::invalid_argument("OAuth retry limit must be between 1 and 16");
    const bool registered_credentials =
        (!options.oauth.client_id.empty() && !options.oauth.client_id.starts_with("https://")) || !options.oauth.client_secret.empty();
    if (registered_credentials && options.oauth.credential_issuer.empty())
      throw std::invalid_argument("Pre-registered OAuth credentials require credential_issuer");
    if (!options.oauth.exchange)
      options.oauth.exchange = curl_auth_exchange(options.http.ca_file);
    if (options.oauth.resource.empty())
      options.oauth.resource = options.http.url;
    options.http.capture_authorization_challenges = true;
    active.store(std::make_shared<HttpTransport>(options.http));
  }
  void authenticate(const HttpAuthorizationError& challenge, RequestOptions request, std::chrono::steady_clock::time_point deadline) {
    auto opts = options.oauth;
    // A URL client identifier can use CIMD; otherwise discover first with a placeholder
    // and register a real native client before requesting an authorization code.
    if (opts.client_id.empty())
      opts.client_id = "https://client.invalid/metadata.json";
    const auto underlying = opts.exchange;
    opts.exchange = [underlying, deadline](const AuthRequest& req, RequestOptions controls) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0)
        throw TransportError(TransportErrorKind::timeout, "OAuth deadline exceeded");
      controls.timeout = (std::min)(controls.timeout, left);
      return underlying(req, controls);
    };
    const auto found = challenge.headers.find("www-authenticate");
    if (found == challenge.headers.end())
      throw AuthError("missing_challenge", "Missing WWW-Authenticate header");
    const auto metadata_url = challenge_field(found->second, "resource_metadata");
    auto client = std::make_unique<OAuthClient>(opts);
    const auto metadata = client->discover(metadata_url, 0, request);
    const auto resource = client->resource_metadata();
    auto scopes = challenge_field(found->second, "scope");
    if (scopes.empty() && resource.contains("scopes_supported"))
      for (const auto& scope : resource["scopes_supported"])
        scopes = scopes_union(scopes, text(scope));
    scopes = scopes_union(scopes_union(granted_scopes, opts.scopes), scopes);
    const bool cimd = metadata.value("client_id_metadata_document_supported", false);
    if (options.oauth.client_id.empty() || (options.oauth.credential_issuer.empty() && !cimd && options.oauth.client_id.starts_with("https://"))) {
      const auto methods = metadata.value("token_endpoint_auth_methods_supported", Json::array({"client_secret_basic"}));
      auto has = [&](const char* method) {
        return methods.is_array() && std::find(methods.begin(), methods.end(), Json(method)) != methods.end();
      };
      const std::string method = has("none")                  ? "none"
                                 : has("client_secret_basic") ? "client_secret_basic"
                                 : has("client_secret_post")  ? "client_secret_post"
                                                              : "";
      if (method.empty())
        throw AuthError("unsupported_token_authentication", "No supported token authentication method");
      Json registration = {{"client_name", "MCP native client"},
                           {"application_type", "native"},
                           {"redirect_uris", {opts.redirect_uri}},
                           {"response_types", {"code"}},
                           {"grant_types", {"authorization_code", "refresh_token"}},
                           {"token_endpoint_auth_method", method}};
      client->register_client(std::move(registration), request);
    }
    if (!options.authorize)
      throw AuthError("authorization_required", "An authorization callback is required");
    auto session = client->authorize(scopes);
    auto callback = options.authorize(session, request);
    const auto tokens = client->complete(session, callback, request);
    granted_scopes = json_text(tokens, "scope");
    if (granted_scopes.empty())
      granted_scopes = scopes;
    const auto identifier = std::string(text(resource["resource"]));
    (void)client->bearer_token(identifier, request);
    auto owner = std::shared_ptr<OAuthClient>(std::move(client));
    auto http = options.http;
    http.bearer_token = [owner, identifier](std::string_view, RequestOptions controls) {
      return owner->bearer_token(identifier, controls);
    };
    auto replacement = std::make_shared<HttpTransport>(std::move(http));
    for (const auto& [name, schema] : schemas)
      replacement->set_tool_schema(name, schema);
    active.store(replacement);
    if (closed.load()) {
      replacement->close();
      throw TransportError(TransportErrorKind::closed, "OAuth transport closed");
    }
  }
};
OAuthTransport::OAuthTransport(OAuthTransportOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
OAuthTransport::~OAuthTransport() {
  close();
}
Json OAuthTransport::exchange(const Json& request, const RequestOptions& options, NotificationHandler notify) {
  if (std::find(authorizing.begin(), authorizing.end(), impl_.get()) != authorizing.end())
    throw AuthError("reentrant_authorization", "Do not issue MCP requests from an authorization callback on the same transport");
  const auto deadline = detail::request_deadline(options.timeout);
  for (std::size_t attempt = 0;; ++attempt) {
    if (impl_->closed.load())
      throw TransportError(TransportErrorKind::closed, "OAuth transport closed");
    auto controls = options;
    controls.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (controls.timeout.count() <= 0)
      throw TransportError(TransportErrorKind::timeout, "OAuth deadline exceeded");
    const auto active = impl_->active.load();
    try {
      return active->exchange(request, controls, notify);
    } catch (const HttpAuthorizationError& error) {
      if (attempt >= impl_->options.max_authorization_attempts)
        throw;
      std::lock_guard lock(impl_->gate);
      if (impl_->active.load() == active) {
        AuthorizationScope authorization(impl_.get());
        impl_->authenticate(error, controls, deadline);
      }
    }
  }
}
void OAuthTransport::set_tool_schema(std::string_view name, const Json& schema) {
  std::lock_guard lock(impl_->gate);
  auto schemas = impl_->schemas;
  schemas[std::string(name)] = schema;
  impl_->active.load()->set_tool_schema(name, schema);
  impl_->schemas = std::move(schemas);
}
void OAuthTransport::set_tool_schemas(const Json& tools) {
  std::lock_guard lock(impl_->gate);
  std::map<std::string, Json, std::less<>> schemas;
  for (const auto& tool : tools)
    schemas[std::string(text(tool["name"]))] = tool["inputSchema"];
  impl_->active.load()->set_tool_schemas(tools);
  impl_->schemas = std::move(schemas);
}
void OAuthTransport::close() noexcept {
  if (impl_) {
    impl_->closed.store(true);
    if (auto transport = impl_->active.load())
      transport->close();
  }
}
}  // namespace mcp
