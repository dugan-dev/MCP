#pragma once

#include <mcp/protocol.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace mcp {

class AuthError : public std::runtime_error {
 public:
  AuthError(std::string code, std::string message);
  const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

struct AuthRequest {
  std::string url;
  std::string method = "GET";
  std::map<std::string, std::string, std::less<>> headers;
  std::string body;
};

struct AuthResponse {
  unsigned status = 0;
  Json body;
  std::map<std::string, std::string, std::less<>> headers;
};

using AuthExchange = std::function<AuthResponse(const AuthRequest&, const RequestOptions&)>;
AuthExchange curl_auth_exchange(std::string ca_file = {}, std::size_t max_bytes = 1024 * 1024);
std::string form_encode(const std::map<std::string, std::string, std::less<>>&);
std::vector<std::string> authorization_metadata_urls(std::string_view issuer);
std::string protected_resource_metadata_url(std::string_view resource);
Json client_metadata_document(std::string_view client_id, std::vector<std::string> redirect_uris, std::string_view name = {});
struct OAuthClientOptions {
  std::string resource;
  std::string client_id;
  std::string redirect_uri;
  std::string client_secret;
  std::string scopes;
  AuthExchange exchange;
  std::function<bool(std::string_view)> allow_url;
  std::function<std::optional<Json>(std::string_view)> load_tokens;
  std::function<void(std::string_view, const Json&)> save_tokens;
  std::function<std::string(std::string_view)> client_assertion;
  std::string credential_issuer;
};

struct AuthorizationSession {
  std::string url;
  std::string state;
  std::string verifier;
  std::string issuer;
  std::string redirect_uri;
  std::string client_id;
  std::chrono::system_clock::time_point expires_at;
};

class OAuthClient final {
 public:
  explicit OAuthClient(OAuthClientOptions);
  ~OAuthClient();
  OAuthClient(const OAuthClient&) = delete;
  OAuthClient& operator=(const OAuthClient&) = delete;

  // Discovery and client registration.
  Json discover(std::string metadata_url = {}, std::size_t issuer_index = 0, RequestOptions = {});
  Json resource_metadata() const;
  Json register_client(Json metadata, RequestOptions = {});

  // Authorization and token lifetime.
  AuthorizationSession authorize(std::string scopes = {});
  Json complete(const AuthorizationSession&, const Json& callback, RequestOptions = {});
  Json client_credentials(RequestOptions = {});
  Json enterprise_exchange(std::string idp_token_endpoint, std::string subject_token, std::string subject_token_type, RequestOptions = {});
  std::string bearer_token(std::string_view resource, RequestOptions = {});
  void clear_tokens();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct VerifiedToken {
  std::string issuer;
  std::vector<std::string> audiences;
  std::vector<std::string> scopes;
  std::chrono::system_clock::time_point expires_at;
  Json attributes = Json::object();
};

struct ResourceAuthorizationOptions {
  std::string resource;
  std::vector<std::string> authorization_servers;
  std::vector<std::string> scopes;
  std::function<std::optional<VerifiedToken>(std::string_view)> verify;
};

struct AuthorizationDecision {
  unsigned status = 401;
  std::string challenge;
  Json attributes;
};

class ResourceAuthorization final {
 public:
  explicit ResourceAuthorization(ResourceAuthorizationOptions);
  Json metadata() const;
  std::string metadata_path() const;
  AuthorizationDecision authenticate(std::string_view authorization) const;

 private:
  ResourceAuthorizationOptions options_;
};

}  // namespace mcp
