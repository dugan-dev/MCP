#include <algorithm>
#include <array>
#include <climits>
#include <curl/curl.h>
#include <map>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <mcp/auth.h>

namespace mcp {
namespace {
using Fields = std::map<std::string, std::string, std::less<>>;
void require(bool condition, std::string_view code = "invalid_metadata") {
    if (!condition)
        throw AuthError(std::string(code), "OAuth validation failed");
}
std::string string(const Json &value, std::string_view field) {
    require(value.is_object() && value.contains(field) && value[field].is_string());
    return std::string(text(value[field]));
}
bool ascii_equal(std::string_view a, std::string_view b) {
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [&](unsigned char x, unsigned char y) { return lower(x) == lower(y); });
}
bool valid_bearer(std::string_view token) {
    return !token.empty() && std::none_of(token.begin(), token.end(),
        [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
}
struct Url {
    std::string origin, path, host, scheme;
    explicit Url(std::string_view value) {
        auto *raw = curl_url();
        if (!raw)
            throw std::bad_alloc();
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> owner(raw, curl_url_cleanup);
        const std::string input(value);
        require(value.find_first_of("\r\n\0", 0, 3) == std::string_view::npos &&
                    curl_url_set(raw, CURLUPART_URL, input.c_str(), 0) == CURLUE_OK,
                "invalid_url");
        auto get = [&](CURLUPart part) {
            char *p = nullptr;
            if (curl_url_get(raw, part, &p, 0) != CURLUE_OK)
                return std::string{};
            std::unique_ptr<char, decltype(&curl_free)> ptr(p, curl_free);
            return std::string(p);
        };
        scheme = get(CURLUPART_SCHEME);
        host = get(CURLUPART_HOST);
        path = get(CURLUPART_PATH);
        require((scheme == "https" || scheme == "http") && !host.empty() &&
                    get(CURLUPART_USER).empty() && get(CURLUPART_PASSWORD).empty() &&
                    get(CURLUPART_FRAGMENT).empty(),
                "invalid_url");
        const auto port = get(CURLUPART_PORT);
        origin = scheme + "://" + host + (port.empty() ? "" : ":" + port);
    }
    bool secure() const {
        return scheme == "https" || host == "127.0.0.1" || host == "[::1]" || host == "localhost";
    }
};
std::string random() {
    std::array<unsigned char, 32> data{};
    if (RAND_bytes(data.data(), static_cast<int>(data.size())) != 1)
        throw AuthError("entropy_failure", "Cannot generate OAuth nonce");
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto byte : data) {
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}
std::string challenge(std::string_view verifier) {
    std::array<unsigned char, 32> digest{};
    unsigned length = 0;
    require(EVP_Digest(verifier.data(), verifier.size(), digest.data(), &length, EVP_sha256(),
                       nullptr) == 1);
    std::array<unsigned char, 48> encoded{};
    const auto size = EVP_EncodeBlock(encoded.data(), digest.data(), static_cast<int>(length));
    std::string result(reinterpret_cast<char *>(encoded.data()), static_cast<std::size_t>(size));
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (result.ends_with('='))
        result.pop_back();
    return result;
}
bool contains(const Json &array, std::string_view value) {
    return array.is_array() && std::find(array.begin(), array.end(), Json(value)) != array.end();
}
std::string joined(const std::vector<std::string> &items) {
    std::string result;
    for (const auto &item : items) {
        require(!item.empty() && item.find_first_of(" \"\\\r\n\t") == std::string::npos);
        if (!result.empty())
            result += ' ';
        result += item;
    }
    return result;
}
std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace
AuthError::AuthError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}
std::string form_encode(const Fields &fields) {
    static constexpr char hex[] = "0123456789ABCDEF";
    auto encode = [&](std::string_view value) {
        std::string result;
        for (unsigned char ch : value) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~')
                result += char(ch);
            else {
                result += '%';
                result += hex[ch >> 4];
                result += hex[ch & 15];
            }
        }
        return result;
    };
    std::string result;
    for (const auto &[key, value] : fields) {
        if (!result.empty())
            result += '&';
        result += encode(key) + '=' + encode(value);
    }
    return result;
}
std::vector<std::string> authorization_metadata_urls(std::string_view issuer) {
    Url url(issuer);
    require(url.secure() && issuer.find('?') == std::string_view::npos, "invalid_issuer");
    const auto suffix = url.path == "/" ? "" : url.path;
    return {url.origin + "/.well-known/oauth-authorization-server" + suffix,
            url.origin + "/.well-known/openid-configuration" + suffix,
            url.origin + suffix + "/.well-known/openid-configuration"};
}
std::string protected_resource_metadata_url(std::string_view resource) {
    Url url(resource);
    require(url.secure() && resource.find('?') == std::string_view::npos, "invalid_resource");
    return url.origin + "/.well-known/oauth-protected-resource" + (url.path == "/" ? "" : url.path);
}
Json client_metadata_document(std::string_view id, std::vector<std::string> redirects,
                              std::string_view name) {
    require(Url(id).scheme == "https" && !redirects.empty());
    for (const auto &uri : redirects)
        require(Url(uri).secure());
    Json result = {{"client_id", id},
                   {"redirect_uris", redirects},
                   {"token_endpoint_auth_method", "none"},
                   {"grant_types", {"authorization_code", "refresh_token"}},
                   {"response_types", {"code"}}};
    if (!name.empty())
        result["client_name"] = name;
    return result;
}
AuthExchange curl_auth_exchange(std::string ca_file, std::size_t max_bytes) {
    require(max_bytes > 0);
    static std::once_flag curl_init_once;
    std::call_once(curl_init_once, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw AuthError("curl_init_failed", "Cannot initialize OAuth HTTP client");
    });
    return [ca_file = std::move(ca_file), max_bytes](const AuthRequest &request,
                                                     const RequestOptions &options) {
        require(Url(request.url).secure(), "insecure_endpoint");
        require(options.timeout.count() > 0 && options.timeout.count() <= LONG_MAX,
                "invalid_timeout");
        if (options.stop.stop_requested())
            throw TransportError(TransportErrorKind::cancelled, "OAuth request cancelled");
        auto *raw = curl_easy_init();
        if (!raw)
            throw std::bad_alloc();
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(raw, curl_easy_cleanup);
        curl_slist *headers = nullptr;
        for (const auto &[key, value] : request.headers) {
            require(key.find_first_of(":\r\n\0", 0, 4) == std::string::npos &&
                        value.find_first_of("\r\n\0", 0, 3) == std::string::npos,
                    "invalid_header");
            auto *next = curl_slist_append(headers, (key + ": " + value).c_str());
            if (!next) {
                curl_slist_free_all(headers);
                throw std::bad_alloc();
            }
            headers = next;
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> fields(headers,
                                                                           curl_slist_free_all);
        struct Body {
            std::string value;
            std::size_t limit;
            bool exceeded = false;
        } body{{}, max_bytes};
        auto set = [&](CURLoption option, auto value) {
            require(curl_easy_setopt(raw, option, value) == CURLE_OK, "network_configuration");
        };
        set(CURLOPT_URL, request.url.c_str());
        set(CURLOPT_HTTPHEADER, headers);
        set(CURLOPT_PROTOCOLS_STR, "http,https");
        set(CURLOPT_FOLLOWLOCATION, 0L);
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_SSL_VERIFYPEER, 1L);
        set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_TIMEOUT_MS, static_cast<long>(options.timeout.count()));
        if (!ca_file.empty())
            set(CURLOPT_CAINFO, ca_file.c_str());
        if (request.method == "POST") {
            set(CURLOPT_POST, 1L);
            set(CURLOPT_POSTFIELDS, request.body.c_str());
            set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        } else
            require(request.method == "GET", "invalid_method");
        set(
            CURLOPT_WRITEFUNCTION,
            +[](char *data, std::size_t size, std::size_t count, void *p) noexcept -> std::size_t {
                auto &out = *static_cast<Body *>(p);
                const auto n = size * count;
                if (n > out.limit - out.value.size()) {
                    out.exceeded = true;
                    return 0;
                }
                try {
                    out.value.append(data, n);
                    return n;
                } catch (...) {
                    return 0;
                }
            });
        set(CURLOPT_WRITEDATA, &body);
        set(CURLOPT_NOPROGRESS, 0L);
        set(
            CURLOPT_XFERINFOFUNCTION,
            +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
                return static_cast<const RequestOptions *>(p)->stop.stop_requested() ? 1 : 0;
            });
        set(CURLOPT_XFERINFODATA, &options);
        const auto code = curl_easy_perform(raw);
        if (options.stop.stop_requested())
            throw TransportError(TransportErrorKind::cancelled, "OAuth request cancelled");
        if (body.exceeded)
            throw TransportError(TransportErrorKind::limit, "OAuth response exceeds limit");
        if (code != CURLE_OK)
            throw TransportError(code == CURLE_OPERATION_TIMEDOUT ? TransportErrorKind::timeout
                                                                  : TransportErrorKind::connection,
                                 "OAuth network request failed");
        long status = 0;
        curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &status);
        return AuthResponse{static_cast<unsigned>(status),
                            body.value.empty() ? Json() : parse_json(body.value, max_bytes)};
    };
}
struct OAuthClient::Impl {
    OAuthClientOptions options;
    std::mutex mutex;
    Json metadata, tokens;
    std::string issuer;
    struct Pending {
        std::string verifier, issuer, redirect_uri, client_id;
        std::chrono::system_clock::time_point expires_at;
    };
    std::map<std::string, Pending, std::less<>> pending;
    std::string key() const { return options.resource + "\n" + issuer + "\n" + options.client_id; }
    AuthResponse send(AuthRequest request, RequestOptions opts) {
        require(Url(request.url).secure() && (!options.allow_url || options.allow_url(request.url)),
                "endpoint_denied");
        return options.exchange(request, opts);
    }
    Json token(Fields fields, RequestOptions opts, std::string endpoint = {}) {
        if (endpoint.empty())
            endpoint = string(metadata, "token_endpoint");
        fields["resource"] = options.resource;
        if (!options.scopes.empty() && !fields.contains("scope"))
            fields["scope"] = options.scopes;
        fields["client_id"] = options.client_id;
        if (options.client_assertion) {
            fields["client_assertion_type"] =
                "urn:ietf:params:oauth:client-assertion-type:jwt-bearer";
            fields["client_assertion"] = options.client_assertion(endpoint);
            require(!fields["client_assertion"].empty(), "invalid_client_assertion");
        } else if (!options.client_secret.empty())
            fields["client_secret"] = options.client_secret;
        auto response = send({endpoint,
                              "POST",
                              {{"Content-Type", "application/x-www-form-urlencoded"},
                               {"Accept", "application/json"}},
                              form_encode(fields)},
                             opts);
        if (response.status != 200)
            throw AuthError("token_error", "Authorization server rejected token request");
        return response.body;
    }
    Json save(Json value, bool refreshing = false) {
        require(valid_bearer(string(value, "access_token")) &&
                    ascii_equal(string(value, "token_type"), "Bearer"),
                "invalid_token");
        if (value.contains("refresh_token")) {
            const auto refresh = string(value, "refresh_token");
            require(!refresh.empty() && std::none_of(refresh.begin(), refresh.end(),
                [](unsigned char c) { return c < 0x20 || c == 0x7f; }), "invalid_token");
        }
        if (value.contains("expires_in")) {
            require(value["expires_in"].is_number_integer(), "invalid_token");
            const auto seconds = value["expires_in"].get<std::int64_t>();
            require(seconds > 0 && seconds <= 365 * 24 * 3600, "invalid_token");
            value["expires_at"] = now_seconds() + seconds;
        } else
            value["expires_at"] = now_seconds() + 300;
        if (refreshing && !value.contains("refresh_token") && tokens.is_object() &&
            tokens.contains("refresh_token"))
            value["refresh_token"] = tokens["refresh_token"];
        tokens = std::move(value);
        if (options.save_tokens)
            options.save_tokens(key(), tokens);
        return tokens;
    }
};
OAuthClient::OAuthClient(OAuthClientOptions options) : impl_(std::make_unique<Impl>()) {
    (void)protected_resource_metadata_url(options.resource);
    require(!options.client_id.empty(), "invalid_client");
    if (!options.exchange)
        options.exchange = curl_auth_exchange();
    impl_->options = std::move(options);
}
OAuthClient::~OAuthClient() = default;
Json OAuthClient::discover(std::string metadata_url, std::size_t index, RequestOptions opts) {
    std::lock_guard lock(impl_->mutex);
    if (metadata_url.empty())
        metadata_url = protected_resource_metadata_url(impl_->options.resource);
    require(Url(metadata_url).origin == Url(impl_->options.resource).origin,
            "resource_metadata_origin");
    auto resource = impl_->send({metadata_url, "GET", {}, {}}, opts);
    require(resource.status == 200 && string(resource.body, "resource") == impl_->options.resource);
    const auto &servers = resource.body.at("authorization_servers");
    require(servers.is_array() && index < servers.size() && servers[index].is_string());
    const auto issuer = std::string(text(servers[index]));
    for (const auto &url : authorization_metadata_urls(issuer)) {
        auto response = impl_->send({url, "GET", {}, {}}, opts);
        if (response.status == 404)
            continue;
        require(response.status == 200 && string(response.body, "issuer") == issuer,
                "issuer_mismatch");
        require(Url(string(response.body, "token_endpoint")).secure());
        impl_->metadata = response.body;
        impl_->issuer = issuer;
        impl_->tokens = Json();
        impl_->pending.clear();
        if (impl_->options.load_tokens) {
            if (auto stored = impl_->options.load_tokens(impl_->key())) {
                require(stored->is_object() && stored->contains("expires_at") &&
                            (*stored)["expires_at"].is_number_integer(),
                        "invalid_stored_token");
                (void)string(*stored, "access_token");
                impl_->tokens = std::move(*stored);
            }
        }
        return response.body;
    }
    throw AuthError("discovery_failed", "No authorization server metadata found");
}
Json OAuthClient::register_client(Json metadata, RequestOptions opts) {
    std::lock_guard lock(impl_->mutex);
    require(metadata.is_object() && metadata.contains("redirect_uris") &&
                metadata["redirect_uris"].is_array(),
            "invalid_client_metadata");
    for (const auto &uri : metadata["redirect_uris"])
        require(uri.is_string() && Url(text(uri)).secure());
    auto response = impl_->send({string(impl_->metadata, "registration_endpoint"),
                                 "POST",
                                 {{"Content-Type", "application/json"}},
                                 serialize_json(metadata)},
                                opts);
    require(response.status == 201 || response.status == 200, "registration_failed");
    impl_->options.client_id = string(response.body, "client_id");
    if (response.body.contains("client_secret"))
        impl_->options.client_secret = string(response.body, "client_secret");
    return response.body;
}
AuthorizationSession OAuthClient::authorize(std::string scopes) {
    std::lock_guard lock(impl_->mutex);
    require(contains(impl_->metadata.value("code_challenge_methods_supported", Json()), "S256"),
            "pkce_required");
    require(Url(impl_->options.redirect_uri).secure(), "invalid_redirect_uri");
    AuthorizationSession session{{},
                                 random(),
                                 random(),
                                 impl_->issuer,
                                 impl_->options.redirect_uri,
                                 impl_->options.client_id,
                                 std::chrono::system_clock::now() + std::chrono::minutes(10)};
    for (auto it = impl_->pending.begin(); it != impl_->pending.end();) {
        if (it->second.expires_at <= std::chrono::system_clock::now())
            it = impl_->pending.erase(it);
        else
            ++it;
    }
    require(impl_->pending.size() < 1024, "too_many_sessions");
    Fields fields = {{"response_type", "code"},
                     {"client_id", session.client_id},
                     {"redirect_uri", session.redirect_uri},
                     {"state", session.state},
                     {"code_challenge", challenge(session.verifier)},
                     {"code_challenge_method", "S256"},
                     {"resource", impl_->options.resource}};
    if (scopes.empty())
        scopes = impl_->options.scopes;
    if (!scopes.empty())
        fields["scope"] = std::move(scopes);
    session.url = string(impl_->metadata, "authorization_endpoint");
    require(Url(session.url).secure());
    session.url += (session.url.find('?') == std::string::npos ? '?' : '&') + form_encode(fields);
    impl_->pending.emplace(session.state,
                           Impl::Pending{session.verifier, session.issuer, session.redirect_uri,
                                         session.client_id, session.expires_at});
    return session;
}
Json OAuthClient::complete(const AuthorizationSession &session, const Json &callback,
                           RequestOptions opts) {
    std::lock_guard lock(impl_->mutex);
    const auto pending = impl_->pending.find(session.state);
    require(pending != impl_->pending.end() && pending->second.verifier == session.verifier &&
                pending->second.issuer == session.issuer &&
                pending->second.redirect_uri == session.redirect_uri &&
                pending->second.client_id == session.client_id &&
                pending->second.expires_at == session.expires_at,
            "invalid_session");
    require(session.issuer == impl_->issuer && session.client_id == impl_->options.client_id &&
                session.redirect_uri == impl_->options.redirect_uri &&
                session.expires_at > std::chrono::system_clock::now(),
            "invalid_session");
    const auto state = string(callback, "state");
    require(state.size() == session.state.size() &&
                CRYPTO_memcmp(state.data(), session.state.data(), state.size()) == 0,
            "state_mismatch");
    if (callback.contains("iss"))
        require(string(callback, "iss") == session.issuer, "issuer_mismatch");
    else
        require(!impl_->metadata.value("authorization_response_iss_parameter_supported", false),
                "missing_issuer");
    require(!callback.contains("error"), "authorization_denied");
    impl_->pending.erase(pending);
    return impl_->save(impl_->token({{"grant_type", "authorization_code"},
                                     {"code", string(callback, "code")},
                                     {"code_verifier", session.verifier},
                                     {"redirect_uri", session.redirect_uri}},
                                    opts));
}
Json OAuthClient::client_credentials(RequestOptions opts) {
    std::lock_guard lock(impl_->mutex);
    require(!impl_->options.client_secret.empty() || bool(impl_->options.client_assertion),
            "client_auth_required");
    return impl_->save(impl_->token({{"grant_type", "client_credentials"}}, opts));
}
Json OAuthClient::enterprise_exchange(std::string endpoint, std::string subject, std::string type,
                                      RequestOptions opts) {
    std::lock_guard lock(impl_->mutex);
    require(type == "urn:ietf:params:oauth:token-type:id_token" ||
                type == "urn:ietf:params:oauth:token-type:refresh_token" ||
                type == "urn:ietf:params:oauth:token-type:saml2",
            "invalid_subject_token_type");
    auto assertion =
        impl_->token({{"grant_type", "urn:ietf:params:oauth:grant-type:token-exchange"},
                      {"requested_token_type", "urn:ietf:params:oauth:token-type:id-jag"},
                      {"audience", impl_->issuer},
                      {"subject_token", std::move(subject)},
                      {"subject_token_type", std::move(type)}},
                     opts, std::move(endpoint));
    require(string(assertion, "issued_token_type") == "urn:ietf:params:oauth:token-type:id-jag",
            "invalid_identity_assertion");
    return impl_->save(impl_->token({{"grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer"},
                                     {"assertion", string(assertion, "access_token")}},
                                    opts));
}
std::string OAuthClient::bearer_token(std::string_view resource, RequestOptions opts) {
    std::lock_guard lock(impl_->mutex);
    require(resource == impl_->options.resource, "resource_mismatch");
    require(impl_->tokens.is_object(), "authorization_required");
    if (impl_->tokens.value("expires_at", std::int64_t(0)) <= now_seconds() + 30) {
        const auto refresh = string(impl_->tokens, "refresh_token");
        impl_->save(
            impl_->token({{"grant_type", "refresh_token"}, {"refresh_token", refresh}}, opts), true);
    }
    return string(impl_->tokens, "access_token");
}
void OAuthClient::clear_tokens() {
    std::lock_guard lock(impl_->mutex);
    impl_->tokens = Json();
    if (impl_->options.save_tokens)
        impl_->options.save_tokens(impl_->key(), Json());
}
ResourceAuthorization::ResourceAuthorization(ResourceAuthorizationOptions options)
    : options_(std::move(options)) {
    (void)protected_resource_metadata_url(options_.resource);
    require(!options_.authorization_servers.empty() && bool(options_.verify));
    for (const auto &issuer : options_.authorization_servers)
        (void)authorization_metadata_urls(issuer);
    (void)joined(options_.scopes);
}
Json ResourceAuthorization::metadata() const {
    return {{"resource", options_.resource},
            {"authorization_servers", options_.authorization_servers},
            {"scopes_supported", options_.scopes},
            {"bearer_methods_supported", {"header"}}};
}
std::string ResourceAuthorization::metadata_path() const {
    return Url(protected_resource_metadata_url(options_.resource)).path;
}
AuthorizationDecision ResourceAuthorization::authenticate(std::string_view authorization) const {
    const auto base =
        "Bearer resource_metadata=\"" + protected_resource_metadata_url(options_.resource) + "\"";
    AuthorizationDecision decision{401, base, Json()};
    const auto scopes = joined(options_.scopes);
    if (!scopes.empty())
        decision.challenge += ", scope=\"" + scopes + "\"";
    if (authorization.size() < 7 || authorization[6] != ' ' ||
        !ascii_equal(authorization.substr(0, 6), "Bearer"))
        return decision;
    auto token = authorization.substr(7);
    while (token.starts_with(' '))
        token.remove_prefix(1);
    if (!valid_bearer(token))
        return decision;
    std::optional<VerifiedToken> verified;
    try {
        verified = options_.verify(token);
    } catch (...) {
        return decision;
    }
    if (!verified || verified->expires_at <= std::chrono::system_clock::now() ||
        std::find(options_.authorization_servers.begin(), options_.authorization_servers.end(),
                  verified->issuer) == options_.authorization_servers.end() ||
        std::find(verified->audiences.begin(), verified->audiences.end(), options_.resource) ==
            verified->audiences.end()) {
        decision.challenge += ", error=\"invalid_token\"";
        return decision;
    }
    for (const auto &scope : options_.scopes)
        if (std::find(verified->scopes.begin(), verified->scopes.end(), scope) ==
            verified->scopes.end()) {
            decision.status = 403;
            decision.challenge += ", error=\"insufficient_scope\"";
            return decision;
        }
    require(verified->attributes.is_object(), "invalid_identity");
    decision.status = 200;
    decision.attributes = verified->attributes;
    decision.challenge.clear();
    return decision;
}
} // namespace mcp
