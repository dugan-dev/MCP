#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <condition_variable>
#include <gtest/gtest.h>
#include <thread>
#include <mcp/auth.h>
#include <mcp/client.h>
#include <mcp/http.h>

using namespace mcp;
namespace {
constexpr std::string_view resource_url = "https://api.example/mcp";
constexpr std::string_view issuer_url = "https://login.example/tenant";
constexpr std::string_view token_url = "https://login.example/tenant/token";

struct ExchangeLog {
    std::mutex mutex;
    std::vector<AuthRequest> requests;
    std::function<AuthResponse(const AuthRequest &)> reply;
    AuthResponse operator()(const AuthRequest &request, const RequestOptions &) {
        std::lock_guard lock(mutex);
        requests.push_back(request);
        return reply(request);
    }
    std::vector<AuthRequest> copy() {
        std::lock_guard lock(mutex);
        return requests;
    }
};

Json tokens(std::string access = "access-1", std::string refresh = "refresh-1", int expiry = 3600) {
    return {{"access_token", access},
            {"token_type", "Bearer"},
            {"refresh_token", refresh},
            {"expires_in", expiry}};
}

struct HttpReply {
    unsigned status;
    std::string challenge;
    std::string body;
};
HttpReply http_request(std::uint16_t port, boost::beast::http::verb method, std::string target,
                       std::map<std::string, std::string> headers = {}, std::string body = {}) {
    namespace net = boost::asio;
    namespace http = boost::beast::http;
    net::io_context io;
    net::ip::tcp::socket socket(io);
    socket.connect({net::ip::make_address("127.0.0.1"), port});
    http::request<http::string_body> request(method, target, 11);
    request.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    request.keep_alive(false);
    for (const auto &[key, value] : headers)
        request.set(key, value);
    request.body() = std::move(body);
    request.prepare_payload();
    http::write(socket, request);
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return {response.result_int(), std::string(response[http::field::www_authenticate]),
            response.body()};
}

std::shared_ptr<ExchangeLog> discovery_exchange() {
    auto log = std::make_shared<ExchangeLog>();
    log->reply = [](const AuthRequest &request) -> AuthResponse {
        if (request.url == "https://api.example/.well-known/oauth-protected-resource/mcp")
            return {200, {{"resource", resource_url}, {"authorization_servers", {issuer_url}}}};
        if (request.url == "https://login.example/.well-known/oauth-authorization-server/tenant")
            return {404, Json()};
        if (request.url == "https://login.example/.well-known/openid-configuration/tenant")
            return {200,
                    {{"issuer", issuer_url},
                     {"authorization_endpoint", "https://login.example/tenant/authorize"},
                     {"token_endpoint", token_url},
                     {"registration_endpoint", "https://login.example/tenant/register"},
                     {"code_challenge_methods_supported", {"S256"}},
                     {"authorization_response_iss_parameter_supported", true}}};
        if (request.url == token_url)
            return {200, tokens()};
        if (request.url == "https://login.example/tenant/register")
            return {201, {{"client_id", "registered-client"}}};
        if (request.url == "https://login.example/token")
            return {200,
                    {{"access_token", "identity-assertion"},
                     {"token_type", "Bearer"},
                     {"issued_token_type", "urn:ietf:params:oauth:token-type:id-jag"}}};
        return {404, Json()};
    };
    return log;
}

OAuthClient make_client(const std::shared_ptr<ExchangeLog> &log,
                        std::function<bool(std::string_view)> allow = {}) {
    OAuthClientOptions options;
    options.resource = std::string(resource_url);
    options.client_id = "https://client.example/metadata.json";
    options.redirect_uri = "http://127.0.0.1:43123/callback";
    options.exchange = [log](const AuthRequest &request, const RequestOptions &opts) {
        return (*log)(request, opts);
    };
    options.allow_url = std::move(allow);
    return OAuthClient(std::move(options));
}

TEST(Auth, MetadataUrlsAndClientMetadataAreStrictAndComplete) {
    EXPECT_EQ(authorization_metadata_urls(issuer_url),
              (std::vector<std::string>{
                  "https://login.example/.well-known/oauth-authorization-server/tenant",
                  "https://login.example/.well-known/openid-configuration/tenant",
                  "https://login.example/tenant/.well-known/openid-configuration"}));
    EXPECT_EQ(protected_resource_metadata_url(resource_url),
              "https://api.example/.well-known/oauth-protected-resource/mcp");
    auto document = client_metadata_document("https://client.example/metadata.json",
                                             {"http://127.0.0.1:43123/callback"}, "Example");
    EXPECT_EQ(document["token_endpoint_auth_method"], std::string("none"));
    EXPECT_EQ(document["grant_types"], Json({"authorization_code", "refresh_token"}));
    EXPECT_EQ(document["response_types"], Json({"code"}));
    EXPECT_THROW(client_metadata_document("http://client.example/metadata.json",
                                          {"https://client.example/cb"}),
                 AuthError);
    EXPECT_THROW(authorization_metadata_urls("https://user:secret@login.example"), AuthError);
    EXPECT_THROW(protected_resource_metadata_url("https://api.example/mcp?access_token=secret"),
                 AuthError);
}

TEST(Auth, CIMDRegistrationUsesValidatedMetadataAndReturnedClientId) {
    auto log = discovery_exchange();
    auto client = make_client(log);
    client.discover();
    auto metadata =
        client_metadata_document("https://client.example/metadata.json",
                                 {"http://127.0.0.1:43123/callback"}, "MCP test client");
    EXPECT_EQ(client.register_client(metadata)["client_id"], std::string("registered-client"));
    const auto session = client.authorize();
    EXPECT_NE(session.url.find("client_id=registered-client"), std::string::npos);
    const auto calls = log->copy();
    ASSERT_EQ(calls.size(), 4U);
    EXPECT_EQ(calls[3].url, "https://login.example/tenant/register");
    EXPECT_EQ(parse_json(calls[3].body)["token_endpoint_auth_method"], std::string("none"));
}

TEST(Auth, DiscoveryPinsIssuerAndAppliesEndpointPolicy) {
    auto log = discovery_exchange();
    auto client = make_client(log, [](std::string_view url) {
        return url.find("login.example") != std::string_view::npos ||
               url.find("api.example") != std::string_view::npos;
    });
    const auto metadata = client.discover();
    EXPECT_EQ(metadata["issuer"], std::string(issuer_url));
    const auto calls = log->copy();
    ASSERT_EQ(calls.size(), 3U);
    EXPECT_EQ(calls[0].url, "https://api.example/.well-known/oauth-protected-resource/mcp");
    EXPECT_EQ(calls[1].url, "https://login.example/.well-known/oauth-authorization-server/tenant");
    EXPECT_EQ(calls[2].url, "https://login.example/.well-known/openid-configuration/tenant");

    auto denied_log = discovery_exchange();
    auto denied = make_client(denied_log, [](std::string_view url) {
        return url.find("api.example") != std::string_view::npos;
    });
    EXPECT_THROW(denied.discover(), AuthError);
    EXPECT_EQ(denied_log->copy().size(), 1U);
}

TEST(Auth, AuthorizationUsesS256AndCompletionBindsStateIssuerAndRedirect) {
    auto log = discovery_exchange();
    auto client = make_client(log);
    client.discover();
    auto session = client.authorize("read write");
    EXPECT_EQ(session.issuer, issuer_url);
    EXPECT_EQ(session.redirect_uri, "http://127.0.0.1:43123/callback");
    EXPECT_NE(session.verifier, "");
    EXPECT_EQ(session.verifier.size(), 64U);
    EXPECT_NE(session.url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_NE(session.url.find("code_challenge="), std::string::npos);
    EXPECT_NE(session.url.find("state=" + session.state), std::string::npos);
    EXPECT_NE(session.url.find("scope=read%20write"), std::string::npos);

    Json callback = {{"state", session.state}, {"iss", issuer_url}, {"code", "authorization-code"}};
    auto result = client.complete(session, callback);
    EXPECT_EQ(result["access_token"], std::string("access-1"));
    auto calls = log->copy();
    ASSERT_EQ(calls.size(), 4U);
    EXPECT_EQ(calls.back().url, token_url);
    EXPECT_NE(calls.back().body.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(calls.back().body.find("code_verifier=" + session.verifier), std::string::npos);
    EXPECT_NE(calls.back().body.find("code=authorization-code"), std::string::npos);
    EXPECT_NE(calls.back().body.find("resource=https%3A%2F%2Fapi.example%2Fmcp"),
              std::string::npos);

    auto wrong_state = client.authorize();
    EXPECT_THROW(
        client.complete(wrong_state, {{"state", "wrong"}, {"iss", issuer_url}, {"code", "x"}}),
        AuthError);
    auto wrong_issuer = client.authorize();
    EXPECT_THROW(client.complete(wrong_issuer, {{"state", wrong_issuer.state},
                                                {"iss", "https://attacker.example"},
                                                {"code", "x"}}),
                 AuthError);
    auto missing_issuer = client.authorize();
    EXPECT_THROW(client.complete(missing_issuer, {{"state", missing_issuer.state}, {"code", "x"}}),
                 AuthError);
    auto expired = client.authorize();
    expired.expires_at = std::chrono::system_clock::now() - std::chrono::seconds(1);
    EXPECT_THROW(
        client.complete(expired, {{"state", expired.state}, {"iss", issuer_url}, {"code", "x"}}),
        AuthError);
}

TEST(Auth, AuthorizationCodeSessionCanOnlyBeCompletedOnce) {
    auto log = discovery_exchange();
    auto client = make_client(log);
    client.discover();
    const auto session = client.authorize();
    const Json callback = {{"state", session.state}, {"iss", issuer_url}, {"code", "one-use-code"}};
    ASSERT_NO_THROW(client.complete(session, callback));
    const auto request_count = log->copy().size();
    EXPECT_THROW(client.complete(session, callback), AuthError);
    EXPECT_EQ(log->copy().size(), request_count);
}

TEST(Auth, RefreshIsSingleFlightAndTokenCacheIsResourceBound) {
    auto log = discovery_exchange();
    std::mutex save_mutex;
    int saved = 0;
    OAuthClientOptions options;
    options.resource = std::string(resource_url);
    options.client_id = "client";
    options.exchange = [log](const AuthRequest &request, const RequestOptions &opts) {
        return (*log)(request, opts);
    };
    options.load_tokens = [](std::string_view) -> std::optional<Json> {
        return Json{{"access_token", "old"}, {"refresh_token", "refresh-1"}, {"expires_at", 1}};
    };
    options.save_tokens = [&](std::string_view, const Json &) {
        std::lock_guard lock(save_mutex);
        ++saved;
    };
    OAuthClient client(std::move(options));
    client.discover();
    EXPECT_EQ(client.bearer_token(resource_url), std::string("access-1"));
    EXPECT_EQ(client.bearer_token(resource_url), std::string("access-1"));
    EXPECT_THROW(client.bearer_token("https://other.example/mcp"), AuthError);
    EXPECT_EQ(log->copy().size(), 4U);
    EXPECT_EQ(saved, 1);
    EXPECT_NE(log->copy().back().body.find("grant_type=refresh_token"), std::string::npos);

    auto concurrent_log = discovery_exchange();
    std::atomic<int> refreshes{0};
    concurrent_log->reply = [&](const AuthRequest &request) -> AuthResponse {
        if (request.url == "https://api.example/.well-known/oauth-protected-resource/mcp")
            return {200, {{"resource", resource_url}, {"authorization_servers", {issuer_url}}}};
        if (request.url == "https://login.example/.well-known/oauth-authorization-server/tenant")
            return {404, Json()};
        if (request.url == "https://login.example/.well-known/openid-configuration/tenant")
            return {200, {{"issuer", issuer_url}, {"token_endpoint", token_url}}};
        ++refreshes;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return {200, tokens("fresh")};
    };
    OAuthClientOptions concurrent_options;
    concurrent_options.resource = std::string(resource_url);
    concurrent_options.client_id = "client";
    concurrent_options.exchange = [concurrent_log](const AuthRequest &r, const RequestOptions &o) {
        return (*concurrent_log)(r, o);
    };
    concurrent_options.load_tokens = [](std::string_view) -> std::optional<Json> {
        return Json{{"access_token", "stale"}, {"refresh_token", "refresh"}, {"expires_at", 1}};
    };
    OAuthClient concurrent(std::move(concurrent_options));
    concurrent.discover();
    std::vector<std::thread> workers;
    std::atomic<int> failures{0};
    for (int i = 0; i < 8; ++i)
        workers.emplace_back([&] {
            try {
                EXPECT_EQ(concurrent.bearer_token(resource_url), std::string("fresh"));
            } catch (...) {
                ++failures;
            }
        });
    for (auto &worker : workers)
        worker.join();
    EXPECT_EQ(failures, 0);
    EXPECT_EQ(refreshes, 1);
}

TEST(Auth, ClientCredentialsAndEnterpriseExchangeUseExpectedGrantFields) {
    auto log = discovery_exchange();
    auto client = make_client(log);
    client.discover();
    // Client credentials requires an explicitly configured confidential-client secret.
    OAuthClientOptions options;
    options.resource = std::string(resource_url);
    options.client_id = "client-id";
    options.client_secret = "client-secret";
    options.exchange = [log](const AuthRequest &request, const RequestOptions &opts) {
        return (*log)(request, opts);
    };
    OAuthClient confidential(std::move(options));
    confidential.discover();
    EXPECT_EQ(confidential.client_credentials()["access_token"], std::string("access-1"));
    auto requests = log->copy();
    EXPECT_NE(requests.back().body.find("grant_type=client_credentials"), std::string::npos);
    EXPECT_NE(requests.back().body.find("client_secret=client-secret"), std::string::npos);

    EXPECT_THROW(client.enterprise_exchange("https://login.example/token", "subject", "bad-type"),
                 AuthError);
    auto enterprise = client.enterprise_exchange("https://login.example/token", "subject-token",
                                                 "urn:ietf:params:oauth:token-type:id_token");
    EXPECT_EQ(enterprise["access_token"], std::string("access-1"));
    requests = log->copy();
    EXPECT_EQ(requests[requests.size() - 2].url, "https://login.example/token");
    EXPECT_NE(requests[requests.size() - 2].body.find(
                  "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange"),
              std::string::npos);
    EXPECT_NE(requests.back().body.find(
                  "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer"),
              std::string::npos);
}

TEST(Auth, FormEncodingAndTokenValidationDoNotLeakSecrets) {
    EXPECT_EQ(form_encode({{"space key", "a b!"}, {"plus", "+"}}), "plus=%2B&space%20key=a%20b%21");
    auto log = discovery_exchange();
    log->reply = [](const AuthRequest &request) -> AuthResponse {
        if (request.url == "https://api.example/.well-known/oauth-protected-resource/mcp")
            return {200, {{"resource", resource_url}, {"authorization_servers", {issuer_url}}}};
        if (request.url == "https://login.example/.well-known/oauth-authorization-server/tenant")
            return {404, Json()};
        if (request.url == "https://login.example/.well-known/openid-configuration/tenant")
            return {200,
                    {{"issuer", issuer_url},
                     {"token_endpoint", token_url},
                     {"code_challenge_methods_supported", {"S256"}}}};
        return {200, {{"access_token", "not a bearer token"}, {"token_type", "Bearer"}}};
    };
    auto client = make_client(log);
    client.discover();
    OAuthClientOptions options;
    options.resource = std::string(resource_url);
    options.client_id = "client-id";
    options.client_secret = "secret";
    options.exchange = [log](const AuthRequest &request, const RequestOptions &opts) {
        return (*log)(request, opts);
    };
    OAuthClient confidential(std::move(options));
    confidential.discover();
    EXPECT_THROW(confidential.client_credentials(), AuthError);
}

TEST(Auth, NewGrantsReplaceRefreshCredentialsAndInvalidResponsesPreserveTokens) {
    for (const std::string grant : {"authorization_code", "client_credentials", "enterprise"}) {
        SCOPED_TRACE(grant);
        auto log = discovery_exchange();
        const auto discovery = log->reply;
        Json response = tokens("original", "original-refresh");
        log->reply = [&](const AuthRequest &request) -> AuthResponse {
            return request.url == token_url ? AuthResponse{200, response} : discovery(request);
        };
        OAuthClientOptions options;
        options.resource = std::string(resource_url);
        options.client_id = "client";
        options.client_secret = "synthetic-secret";
        options.redirect_uri = "http://127.0.0.1:43123/callback";
        options.exchange = [log](const AuthRequest &request, const RequestOptions &opts) {
            return (*log)(request, opts);
        };
        Json saved;
        options.save_tokens = [&](std::string_view, const Json &value) { saved = value; };
        OAuthClient client(std::move(options));
        client.discover();
        client.client_credentials();
        const auto acquire = [&]() {
            if (grant == "client_credentials")
                return client.client_credentials();
            if (grant == "enterprise")
                return client.enterprise_exchange("https://login.example/token", "subject",
                                                   "urn:ietf:params:oauth:token-type:id_token");
            auto session = client.authorize();
            return client.complete(session, {{"state", session.state}, {"iss", issuer_url},
                                             {"code", "new-authorization"}});
        };
        for (Json invalid : {Json{{"access_token", ""}, {"token_type", "Bearer"}},
                             Json{{"access_token", "bad\ttoken"}, {"token_type", "Bearer"}},
                             Json{{"access_token", "replacement"}, {"token_type", "Basic"}},
                             Json{{"access_token", "replacement"}, {"token_type", "Bearer"},
                                  {"refresh_token", 12}},
                             Json{{"access_token", "replacement"}, {"token_type", "Bearer"},
                                  {"expires_in", 0}}}) {
            response = std::move(invalid);
            EXPECT_THROW(acquire(), AuthError);
            EXPECT_EQ(client.bearer_token(resource_url), "original");
            EXPECT_EQ(saved["refresh_token"], "original-refresh");
        }
        response = {{"access_token", "replacement"}, {"token_type", "Bearer"}, {"expires_in", 1}};
        auto replaced = acquire();
        EXPECT_FALSE(replaced.contains("refresh_token"));
        EXPECT_FALSE(saved.contains("refresh_token"));
        const auto calls = log->copy().size();
        EXPECT_THROW(client.bearer_token(resource_url), AuthError);
        EXPECT_EQ(log->copy().size(), calls);
    }
}

TEST(Auth, RefreshRetainsOmittedRefreshTokenAndAcceptsCaseInsensitiveBearerType) {
    auto log = discovery_exchange();
    const auto discovery = log->reply;
    int refreshes = 0;
    log->reply = [&](const AuthRequest &request) -> AuthResponse {
        if (request.url != token_url)
            return discovery(request);
        ++refreshes;
        EXPECT_NE(request.body.find("refresh_token=existing-refresh"), std::string::npos);
        return {200, {{"access_token", "Fresh.Exact+Token/="}, {"token_type", "bEaReR"},
                      {"expires_in", 1}}};
    };
    OAuthClientOptions options;
    options.resource = std::string(resource_url);
    options.client_id = "client";
    options.exchange = [log](const AuthRequest &r, const RequestOptions &o) { return (*log)(r, o); };
    options.load_tokens = [](std::string_view) -> std::optional<Json> {
        return Json{{"access_token", "expired"}, {"refresh_token", "existing-refresh"},
                    {"expires_at", 1}};
    };
    Json saved;
    options.save_tokens = [&](std::string_view, const Json &value) { saved = value; };
    OAuthClient client(std::move(options));
    client.discover();
    EXPECT_EQ(client.bearer_token(resource_url), "Fresh.Exact+Token/=");
    EXPECT_EQ(saved["refresh_token"], "existing-refresh");
    EXPECT_EQ(client.bearer_token(resource_url), "Fresh.Exact+Token/=");
    EXPECT_EQ(refreshes, 2);
}

TEST(Auth, BearerSchemeIsCaseInsensitiveAndTokenBytesRemainExact) {
    ResourceAuthorizationOptions options;
    options.resource = std::string(resource_url);
    options.authorization_servers = {std::string(issuer_url)};
    std::vector<std::string> received;
    options.verify = [&](std::string_view token) -> std::optional<VerifiedToken> {
        received.emplace_back(token);
        if (token != "Exact.Token+Bytes/=")
            return std::nullopt;
        VerifiedToken verified;
        verified.issuer = std::string(issuer_url);
        verified.audiences = {std::string(resource_url)};
        verified.expires_at = std::chrono::system_clock::now() + std::chrono::hours(1);
        return verified;
    };
    ResourceAuthorization auth(std::move(options));
    for (const std::string scheme : {"Bearer", "bearer", "BEARER", "bEaReR"})
        EXPECT_EQ(auth.authenticate(scheme + " Exact.Token+Bytes/=").status, 200U);
    EXPECT_EQ(received, (std::vector<std::string>(4, "Exact.Token+Bytes/=")));
    EXPECT_EQ(auth.authenticate("bearer exact.token+bytes/=").status, 401U);
    const auto count = received.size();
    for (const std::string malformed : {"Basic Exact.Token+Bytes/=", "BearerX Exact.Token+Bytes/=",
                                       "Bearer", "Bearer ", "Bearer bad token", "Bearer bad\ttoken",
                                       "Bearer bad\rtoken", "Bearer bad\ntoken",
                                       "Bearer bad\x01token", "Bearer bad\x7ftoken"})
        EXPECT_EQ(auth.authenticate(malformed).status, 401U);
    EXPECT_EQ(received.size(), count);
}

TEST(Auth, BearerCredentialSeparatorAllowsSpacesWithoutTrimmingTokenBytes) {
    ResourceAuthorizationOptions options;
    options.resource = std::string(resource_url);
    options.authorization_servers = {std::string(issuer_url)};
    int verified_count = 0;
    options.verify = [&](std::string_view token) -> std::optional<VerifiedToken> {
        ++verified_count;
        EXPECT_EQ(token, "signed-Token+Bytes/=");
        VerifiedToken verified;
        verified.issuer = std::string(issuer_url);
        verified.audiences = {std::string(resource_url)};
        verified.expires_at = std::chrono::system_clock::now() + std::chrono::hours(1);
        return verified;
    };
    ResourceAuthorization auth(std::move(options));
    for (const int spaces : {1, 3, 10})
        EXPECT_EQ(auth.authenticate("bEaReR" + std::string(spaces, ' ') + "signed-Token+Bytes/=").status,
                  200U);
    EXPECT_EQ(verified_count, 3);
    const auto valid_count = verified_count;
    for (const auto &invalid : std::vector<std::string>{
             " Bearer signed-Token+Bytes/=", "Bearersigned-Token+Bytes/=",
             "Bearer\tsigned-Token+Bytes/=", "Bearer  \tsigned-Token+Bytes/=",
             "Bearer    ", "Bearer signed-Token+Bytes/= ", "Bearer signed-Token+Bytes/=\t",
             "Bearer signed-Token+Bytes/=\r\n", "Bearer signed Token+Bytes/=",
             std::string("Bearer signed") + '\0' + "Token", "Bearer signed\x7fToken"})
        EXPECT_EQ(auth.authenticate(invalid).status, 401U);
    EXPECT_EQ(verified_count, valid_count);
}

TEST(Auth, ResourceAuthorizationChecksBearerIssuerAudienceExpiryAndScopes) {
    ResourceAuthorizationOptions options;
    options.resource = std::string(resource_url);
    options.authorization_servers = {std::string(issuer_url)};
    options.scopes = {"read", "write"};
    options.verify = [](std::string_view token) -> std::optional<VerifiedToken> {
        if (token == "bad-signature")
            return std::nullopt;
        VerifiedToken value;
        value.issuer = token == "wrong-issuer" ? "https://evil.example" : std::string(issuer_url);
        value.audiences = {token == "wrong-audience" ? "https://other.example"
                                                     : std::string(resource_url)};
        value.scopes = token == "insufficient" ? std::vector<std::string>{"read"}
                                               : std::vector<std::string>{"read", "write"};
        value.expires_at = std::chrono::system_clock::now() + std::chrono::minutes(1);
        value.attributes = {{"sub", "alice"}};
        if (token == "expired")
            value.expires_at = std::chrono::system_clock::now() - std::chrono::seconds(1);
        return value;
    };
    ResourceAuthorization auth(std::move(options));
    EXPECT_EQ(auth.metadata_path(), "/.well-known/oauth-protected-resource/mcp");
    EXPECT_EQ(auth.metadata()["resource"], std::string(resource_url));
    EXPECT_EQ(auth.authenticate("").status, 401U);
    EXPECT_NE(
        auth.authenticate("").challenge.find(
            "resource_metadata=\"https://api.example/.well-known/oauth-protected-resource/mcp\""),
        std::string::npos);
    EXPECT_EQ(auth.authenticate("Basic abc").status, 401U);
    EXPECT_EQ(auth.authenticate("Bearer bad-signature").status, 401U);
    EXPECT_EQ(auth.authenticate("Bearer wrong-issuer").status, 401U);
    EXPECT_EQ(auth.authenticate("Bearer wrong-audience").status, 401U);
    EXPECT_EQ(auth.authenticate("Bearer expired").status, 401U);
    EXPECT_NE(auth.authenticate("Bearer expired").challenge.find("error=\"invalid_token\""),
              std::string::npos);
    auto insufficient = auth.authenticate("Bearer insufficient");
    EXPECT_EQ(insufficient.status, 403U);
    EXPECT_NE(insufficient.challenge.find("insufficient_scope"), std::string::npos);
    auto allowed = auth.authenticate("Bearer valid");
    EXPECT_EQ(allowed.status, 200U);
    EXPECT_EQ(allowed.attributes["sub"], std::string("alice"));
    EXPECT_EQ(auth.authenticate("Bearer valid\r\nX-Evil: yes").status, 401U);
}

TEST(Auth, CurlExchangeEnforcesResponseBoundAndCancellation) {
    namespace net = boost::asio;
    net::io_context io;
    net::ip::tcp::acceptor acceptor(io, {net::ip::make_address("127.0.0.1"), 0});
    const auto port = acceptor.local_endpoint().port();
    std::thread server([&] {
        net::ip::tcp::socket socket(io);
        acceptor.accept(socket);
        boost::system::error_code ec;
        std::array<char, 2048> buffer{};
        while (net::read(socket, net::buffer(buffer), net::transfer_at_least(1), ec) && !ec) {
            const std::string_view request(buffer.data(),
                                           std::min<std::size_t>(buffer.size(), 2048));
            if (request.find("\r\n\r\n") != std::string_view::npos)
                break;
        }
        const std::string body = "{\"ok\":true,\"padding\":\"response-body\"}";
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        net::write(socket, net::buffer(response), ec);
    });
    auto exchange = curl_auth_exchange({}, 8);
    RequestOptions request;
    request.timeout = std::chrono::seconds(2);
    EXPECT_THROW(exchange({"http://127.0.0.1:" + std::to_string(port) + "/token"}, request),
                 TransportError);
    server.join();
}

TEST(Auth, HttpServerPublishesMetadataChallengesAndPassesVerifiedIdentity) {
    ResourceAuthorizationOptions authorization_options;
    authorization_options.resource = std::string(resource_url);
    authorization_options.authorization_servers = {std::string(issuer_url)};
    authorization_options.scopes = {"read", "write"};
    authorization_options.verify = [](std::string_view token) -> std::optional<VerifiedToken> {
        if (token == "bad-signature")
            return std::nullopt;
        VerifiedToken verified;
        verified.issuer = std::string(issuer_url);
        verified.audiences = {token == "wrong-audience" ? "https://other.example"
                                                        : std::string(resource_url)};
        verified.scopes = token == "insufficient" ? std::vector<std::string>{"read"}
                                                  : std::vector<std::string>{"read", "write"};
        verified.expires_at = token == "expired"
                                  ? std::chrono::system_clock::now() - std::chrono::seconds(1)
                                  : std::chrono::system_clock::now() + std::chrono::minutes(5);
        verified.attributes = {{"sub", "alice"}, {"tenant", "blue"}};
        return verified;
    };
    auto authorization = std::make_shared<ResourceAuthorization>(std::move(authorization_options));
    Server app;
    app.add_tool(Tool{"identity", "Return authenticated identity", Json{{"type", "object"}}},
                 [](const Json &, RequestContext &context) {
                     return text_result(serialize_json(context.attributes()));
                 });
    HttpServerOptions server_options;
    server_options.resource_authorization = authorization;
    HttpServer server(app, std::move(server_options));
    server.start();
    const auto port = server.port();
    ASSERT_NE(port, 0);

    const auto metadata = http_request(port, boost::beast::http::verb::get,
                                       "/.well-known/oauth-protected-resource/mcp");
    ASSERT_EQ(metadata.status, 200U) << "metadata path=" << authorization->metadata_path()
                                     << " request target=/.well-known/oauth-protected-resource/mcp"
                                     << " body=" << metadata.body;
    EXPECT_EQ(parse_json(metadata.body)["resource"], std::string(resource_url));
    EXPECT_EQ(parse_json(metadata.body)["authorization_servers"][0], std::string(issuer_url));

    const auto no_token = http_request(port, boost::beast::http::verb::post, "/mcp", {}, "{}");
    ASSERT_EQ(no_token.status, 401U) << "request target=/mcp body=" << no_token.body;
    EXPECT_NE(
        no_token.challenge.find(
            "resource_metadata=\"https://api.example/.well-known/oauth-protected-resource/mcp\""),
        std::string::npos);
    EXPECT_NE(no_token.challenge.find("scope=\"read write\""), std::string::npos);

    const auto wrong_audience = http_request(port, boost::beast::http::verb::post, "/mcp",
                                             {{"Authorization", "Bearer wrong-audience"}}, "{}");
    ASSERT_EQ(wrong_audience.status, 401U) << "request target=/mcp body=" << wrong_audience.body;
    EXPECT_NE(wrong_audience.challenge.find("error=\"invalid_token\""), std::string::npos);
    const auto expired = http_request(port, boost::beast::http::verb::post, "/mcp",
                                      {{"Authorization", "Bearer expired"}}, "{}");
    ASSERT_EQ(expired.status, 401U) << "request target=/mcp body=" << expired.body;
    const auto insufficient = http_request(port, boost::beast::http::verb::post, "/mcp",
                                           {{"Authorization", "Bearer insufficient"}}, "{}");
    ASSERT_EQ(insufficient.status, 403U) << "request target=/mcp body=" << insufficient.body;
    EXPECT_NE(insufficient.challenge.find("error=\"insufficient_scope\""), std::string::npos);

    std::atomic<int> provider_calls{0};
    HttpClientOptions client_options;
    client_options.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    const auto bound_resource = client_options.url;
    client_options.bearer_token = [&, bound_resource](std::string_view requested_resource,
                                                      RequestOptions request_options) {
        EXPECT_EQ(requested_resource, bound_resource);
        EXPECT_GT(request_options.timeout.count(), 0);
        ++provider_calls;
        return std::string("valid");
    };
    Client client(std::make_shared<HttpTransport>(std::move(client_options)));
    EXPECT_NO_THROW(client.discover());
    const auto tool = client.call_tool("identity");
    const auto attributes = parse_json(text(tool["content"][0]["text"]));
    EXPECT_EQ(attributes["sub"], std::string("alice"));
    EXPECT_EQ(attributes["tenant"], std::string("blue"));
    EXPECT_GE(provider_calls, 2);
}
} // namespace
