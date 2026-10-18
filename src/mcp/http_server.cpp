#include "http_headers.h"
#include "http_channels.h"
#include "http_shutdown.h"
#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <charconv>
#include <deque>
#include <thread>
#include <mcp/http.h>

namespace mcp {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
namespace {
thread_local const void *active_listener = nullptr;
struct ListenerScope {
    const void *previous;
    explicit ListenerScope(const void *listener)
        : previous(std::exchange(active_listener, listener)) {}
    ~ListenerScope() { active_listener = previous; }
};
std::string chunk(std::string_view data) {
    char size[2 * sizeof(std::size_t) + 1];
    auto [end, error] = std::to_chars(size, size + sizeof(size), data.size(), 16);
    if (error != std::errc())
        throw std::length_error("Chunk size overflow");
    return std::string(size, end) + "\r\n" + std::string(data) + "\r\n";
}
bool accepts(std::string_view value, std::string_view mime) {
    std::size_t position = 0;
    while (position < value.size()) {
        const auto end = value.find(',', position);
        auto token = value.substr(position, end == std::string_view::npos ? value.size() - position
                                                                          : end - position);
        while (token.starts_with(' ') || token.starts_with('\t'))
            token.remove_prefix(1);
        const auto semi = token.find(';');
        if (semi != std::string_view::npos)
            token = token.substr(0, semi);
        while (token.ends_with(' ') || token.ends_with('\t'))
            token.remove_suffix(1);
        if (detail::lower(token) == mime)
            return true;
        if (end == std::string_view::npos)
            break;
        position = end + 1;
    }
    return false;
}
unsigned status_for(const Json &response) {
    if (!response.contains("error"))
        return 200;
    const auto code = response["error"]["code"].get<int>();
    if (code == -32601)
        return 404;
    return code == -32602 || code == -32600 || code == -32700 || code == -32020 || code == -32021 ||
                   code == -32022
               ? 400
               : 200;
}
} // namespace
struct HttpServer::Impl : std::enable_shared_from_this<Impl> {
    struct Channel {
        virtual ~Channel() = default;
        virtual void cancel() = 0;
        virtual void close_quiescent() noexcept = 0;
    };
    template <class Stream> struct Session;
    Server &server;
    HttpServerOptions options;
    net::io_context io;
    net::executor_work_guard<net::io_context::executor_type> guard{net::make_work_guard(io)};
    net::thread_pool workers;
    tcp::acceptor acceptor{io};
    ssl::context tls{ssl::context::tls_server};
    std::vector<std::thread> threads;
    detail::HttpShutdownThread shutdown;
    detail::HttpShutdownReaper *reaper = nullptr;
    std::atomic<bool> start_ready{false}, start_failed{false};
    std::atomic<bool> shutdown_requested{false}, shutdown_complete{false};
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> pending{0};
    std::mutex lifecycle, channels_mutex;
    std::map<std::uint64_t, std::weak_ptr<Channel>> channels;
    std::uint64_t next_channel = 1;
    std::atomic<std::uint16_t> bound_port{0};
    bool started = false, coordinator_started = false;
    Impl(Server &value, HttpServerOptions opts)
        : server(value), options(std::move(opts)), workers(options.worker_threads) {
        if (!options.io_threads || !options.worker_threads || !options.max_connections ||
            options.idle_timeout.count() <= 0 || options.path.empty() ||
            options.path.front() != '/' ||
            options.certificate_file.empty() != options.private_key_file.empty() ||
            (options.resource_authorization && options.authenticate))
            throw std::invalid_argument("Invalid HTTP listener options");
        if (!options.certificate_file.empty()) {
            tls.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                            ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                            ssl::context::no_tlsv1_1);
            tls.use_certificate_chain_file(options.certificate_file);
            tls.use_private_key_file(options.private_key_file, ssl::context::pem);
        }
    }
    void accept();
    void request_shutdown() noexcept {
        stopping.store(true);
        shutdown_requested.store(true);
        shutdown_requested.notify_all();
    }
    template <class Executor, class Handler>
    void post(Executor executor, Handler &&handler) noexcept {
        try {
            net::post(std::move(executor), std::forward<Handler>(handler));
        } catch (...) {
            request_shutdown();
        }
    }
    void close_channels() noexcept {
        auto close = [](const std::shared_ptr<Channel> &channel) noexcept {
            channel->close_quiescent();
        };
        try {
            auto snapshot = detail::snapshot_live_channels(channels_mutex, channels);
            for (auto &channel : snapshot)
                close(channel);
        } catch (...) {
            detail::visit_live_channels(channels_mutex, channels, close);
        }
    }
    void finish_shutdown() noexcept {
        ListenerScope listener_scope(this);
        JsonScope scope(server.options().memory_resource);
        // No Session IO handler may run concurrently with synchronous close.
        io.stop();
        for (auto &thread : threads)
            if (thread.joinable())
                thread.join();
        boost::system::error_code error;
        acceptor.close(error);
        close_channels();
        // Close delivers cooperative cancellation before waiting for workers.
        workers.join();
        guard.reset();
        io.restart();
        for (;;) {
            try {
                io.run();
                break;
            } catch (...) {
                // All channels are closed and producers joined. Discard this
                // failed completion and continue draining the finite queue.
            }
        }
        shutdown_complete.store(true);
        shutdown_complete.notify_all();
    }
    void erase(std::uint64_t key) {
        std::lock_guard lock(channels_mutex);
        channels.erase(key);
    }
};
template <class Stream>
struct HttpServer::Impl::Session final : Channel, std::enable_shared_from_this<Session<Stream>> {
    std::shared_ptr<Impl> owner;
    Stream stream;
    beast::basic_flat_buffer<JsonAllocator<char>> buffer;
    std::optional<http::request_parser<http::string_body>> parser;
    std::optional<http::response<http::string_body>> response;
    http::response<http::empty_body> stream_header;
    std::optional<http::response_serializer<http::empty_body>> serializer;
    std::deque<String, JsonAllocator<String>> writes;
    std::size_t queued = 0;
    std::atomic<std::size_t> reserved{0};
    std::atomic<bool> overflow{false};
    std::uint64_t key;
    std::stop_source cancellation;
    net::cancellation_signal monitor_cancel;
    std::shared_ptr<Subscription> subscription;
    struct DeferredReply {
        unsigned status;
        Json value;
        bool keep_alive;
    };
    std::optional<DeferredReply> deferred_reply;
    std::atomic<bool> closed{false};
    bool header_written = false, writing = false, final = false;
    bool monitoring = false;
    std::string challenge;
    Json request;
    Json attributes;
    Session(std::shared_ptr<Impl> impl, Stream socket, std::uint64_t id)
        : owner(std::move(impl)), stream(std::move(socket)), key(id) {}
    ~Session() { owner->erase(key); }
    void cancel() override {
        auto self = this->shared_from_this();
        owner->post(stream.get_executor(), [self] { self->close(); });
    }
    void close_quiescent() noexcept override { close(); }
    void close() noexcept {
        if (closed.exchange(true))
            return;
        cancellation.request_stop();
        if (subscription) {
            try {
                subscription->close();
            } catch (...) {
            }
            subscription.reset();
        }
        boost::system::error_code error;
        beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, error);
        beast::get_lowest_layer(stream).socket().close(error);
    }
    void start() {
        auto self = this->shared_from_this();
        if constexpr (std::is_same_v<Stream, beast::ssl_stream<beast::tcp_stream>>) {
            beast::get_lowest_layer(stream).expires_after(owner->options.idle_timeout);
            stream.async_handshake(ssl::stream_base::server,
                                   [self](boost::system::error_code error) {
                                       if (error)
                                           self->close();
                                       else
                                           self->read();
                                   });
        } else
            read();
    }
    void read() {
        if (closed || owner->stopping.load())
            return close();
        request = nullptr;
        challenge.clear();
        attributes = Json::object();
        response.reset();
        serializer.reset();
        writes.clear();
        queued = 0;
        header_written = false;
        final = false;
        cancellation = std::stop_source();
        parser.emplace();
        parser->body_limit(owner->server.options().limits.message_bytes);
        parser->header_limit(65536);
        beast::get_lowest_layer(stream).expires_after(owner->options.idle_timeout);
        auto self = this->shared_from_this();
        http::async_read(
            stream, buffer, *parser, [self](boost::system::error_code error, std::size_t) {
                if (error) {
                    if (error == http::error::body_limit)
                        self->reply(413,
                                    make_error(nullptr, -32600, "Request body exceeds byte limit"),
                                    false);
                    else
                        self->close();
                } else {
                    try {
                        self->received();
                    } catch (...) {
                        self->close();
                    }
                }
            });
    }
    void reply(unsigned status, Json value, bool keep_alive = true) {
        if (closed)
            return;
        if (monitoring) {
            deferred_reply.emplace(DeferredReply{status, std::move(value), keep_alive});
            monitor_cancel.emit(net::cancellation_type::all);
            return;
        }
        response.emplace(static_cast<http::status>(status), 11);
        response->set(http::field::server, "MCP/0.2.0");
        if ((status == 401 || status == 403) && !challenge.empty())
            response->set(http::field::www_authenticate, challenge);
        if (status == 405)
            response->set(http::field::allow, "POST");
        if (status != 202) {
            response->set(http::field::content_type, "application/json");
            response->body() = serialize_json(value);
            if (response->body().size() > owner->server.options().limits.message_bytes)
                return close();
        }
        response->keep_alive(keep_alive && parser && parser->get().keep_alive());
        response->prepare_payload();
        beast::get_lowest_layer(stream).expires_after(owner->options.idle_timeout);
        auto self = this->shared_from_this();
        http::async_write(stream, *response, [self](boost::system::error_code error, std::size_t) {
            if (error || !self->response->keep_alive())
                self->close();
            else
                self->read();
        });
    }
    void finish_deferred_reply() {
        if (!deferred_reply)
            return;
        auto deferred = std::move(*deferred_reply);
        deferred_reply.reset();
        reply(deferred.status, std::move(deferred.value), deferred.keep_alive);
    }
    void received() {
        // Draining IO can deliver an already successful socket read after the
        // worker pool has joined. It must neither authorize nor enqueue work.
        if (closed.load() || owner->stopping.load())
            return close();
        JsonScope scope(owner->server.options().memory_resource);
        auto &incoming = parser->get();
        Headers headers;
        for (const auto &field : incoming) {
            auto name = detail::lower(
                std::string_view(field.name_string().data(), field.name_string().size()));
            if (headers.contains(name) && (name.starts_with("mcp-") || name == "origin" ||
                                           name == "host" || name == "authorization"))
                return reply(400, make_error(nullptr, -32020, "Duplicate HTTP metadata header"));
            headers.insert_or_assign(std::move(name),
                                     std::string(field.value().data(), field.value().size()));
        }
        const auto incoming_host = headers.find("host");
        if (incoming_host == headers.end())
            return reply(400, make_error(nullptr, -32020, "Host header is required"));
        const auto host = detail::lower(incoming_host->second);
        auto accepted_host = [&](std::string_view name) {
            const auto lower = detail::lower(name);
            return host == lower || host == lower + ":" + std::to_string(owner->bound_port.load());
        };
        bool host_ok = accepted_host(owner->options.host);
        if (owner->options.host == "127.0.0.1" || owner->options.host == "::1")
            host_ok = host_ok || accepted_host("localhost") || accepted_host("127.0.0.1") ||
                      accepted_host("[::1]");
        for (const auto &name : owner->options.allowed_hosts)
            host_ok = host_ok || accepted_host(name);
        if (!host_ok)
            return reply(403, make_error(nullptr, 1003, "Host is not allowed"));
        if (auto origin = headers.find("origin");
            origin != headers.end() &&
            std::find(owner->options.allowed_origins.begin(), owner->options.allowed_origins.end(),
                      origin->second) == owner->options.allowed_origins.end())
            return reply(403, make_error(nullptr, 1003, "Origin is not allowed"));
        if (owner->options.resource_authorization && incoming.method() == http::verb::get &&
            incoming.target() == owner->options.resource_authorization->metadata_path())
            return reply(200, owner->options.resource_authorization->metadata());
        if (incoming.target() != owner->options.path)
            return reply(404, make_error(nullptr, -32601, "Unknown endpoint"));
        if (incoming.method() != http::verb::post)
            return reply(405, make_error(nullptr, -32600, "Use POST"));
        if (owner->options.authorize) {
            bool authorized = false;
            try {
                authorized = owner->options.authorize(headers);
            } catch (...) {
            }
            if (!authorized)
                return reply(401, make_error(nullptr, 1004, "Unauthorized"));
        }
        if (owner->options.resource_authorization) {
            const auto authorization = headers.find("authorization");
            const auto decision = owner->options.resource_authorization->authenticate(
                authorization == headers.end() ? std::string_view{}
                                               : std::string_view(authorization->second));
            if (decision.status != 200) {
                challenge = decision.challenge;
                return reply(decision.status, make_error(nullptr, 1004, "Unauthorized"));
            }
            attributes = decision.attributes;
        }
        if (owner->options.authenticate) {
            std::optional<Json> identity;
            try {
                identity = owner->options.authenticate(headers);
            } catch (...) {
            }
            if (!identity)
                return reply(401, make_error(nullptr, 1004, "Unauthorized"));
            attributes = std::move(*identity);
        }
        const auto type = headers.find("content-type");
        if (type == headers.end() || !accepts(type->second, "application/json"))
            return reply(415, make_error(nullptr, -32600, "Expected application/json"));
        const auto accept = headers.find("accept");
        if (accept == headers.end() || !accepts(accept->second, "application/json") ||
            !accepts(accept->second, "text/event-stream"))
            return reply(406, make_error(nullptr, -32600, "Accept JSON and SSE"));
        try {
            request = parse_json(incoming.body(), owner->server.options().limits.message_bytes,
                                 owner->server.options().limits.json_depth);
            if (request.is_object() && !request.contains("id") && request.contains("method") &&
                request["method"].is_string() && request.value("jsonrpc", Json()) == "2.0")
                return reply(202, nullptr);
            if (request.is_object() && request.contains("params") &&
                request["params"].is_object() && request["params"].contains("_meta") &&
                request["params"]["_meta"].is_object() &&
                request["params"]["_meta"].contains(version_key) &&
                request["params"]["_meta"][version_key].is_string()) {
                const auto version = headers.find("mcp-protocol-version");
                if (version == headers.end() ||
                    version->second != text(request["params"]["_meta"][version_key]))
                    throw RpcError(ErrorCode::header_mismatch, "Protocol header/body mismatch");
            }
            validate_request(request);
            Json schema;
            if (request["method"] == "tools/call" && request["params"].contains("name") &&
                request["params"]["name"].is_string())
                schema = owner->server.tool_schema(text(request["params"]["name"]));
            detail::validate_headers(headers, request, schema);
        } catch (const RpcError &error) {
            Json id;
            if (request.is_object() && request.contains("id"))
                id = request["id"];
            return reply(400, make_error(id, error.code(), error.what(), error.data()));
        }
        // Authorization callbacks may themselves request shutdown.
        if (closed.load() || owner->stopping.load())
            return close();
        if (owner->pending.fetch_add(1) >= owner->server.options().limits.pending_requests) {
            owner->pending.fetch_sub(1);
            return reply(503, make_error(request["id"], 1002, "Server is at capacity"));
        }
        beast::get_lowest_layer(stream).expires_never();
        monitor_disconnect();
        auto self = this->shared_from_this();
        owner->post(owner->workers.get_executor(), [self] {
            auto owner = self->owner;
            ListenerScope listener_scope(owner.get());
            struct Pending {
                std::shared_ptr<Impl> owner;
                ~Pending() { owner->pending.fetch_sub(1); }
            } pending{owner};
            JsonScope scope(owner->server.options().memory_resource);
            try {
                std::weak_ptr<Session> weak = self;
                auto send = [weak](const Json &message) {
                    if (auto session = weak.lock()) {
                        session->send(message);
                    }
                };
                if (self->request["method"] == "subscriptions/listen") {
                    auto sub = owner->server.subscribe(self->request, std::move(send), self->attributes);
                    owner->post(self->stream.get_executor(), [self, sub = std::move(sub)]() mutable {
                        if (self->closed)
                            sub->close();
                        else
                            self->subscription = std::move(sub);
                    });
                } else {
                    auto result = owner->server.dispatch(
                        self->request, DispatchOptions{self->cancellation.get_token(),
                                                       std::move(send), self->attributes});
                    owner->post(self->stream.get_executor(),
                              [self, result = std::move(result)]() mutable {
                                  try {
                                      if (self->serializer)
                                          self->send(result);
                                      else {
                                          const auto status = status_for(result);
                                          self->reply(status, std::move(result));
                                      }
                                  } catch (...) {
                                      self->close();
                                  }
                              });
                }
            } catch (const RpcError &error) {
                try {
                    auto result =
                        make_error(self->request["id"], error.code(), error.what(), error.data());
                    owner->post(self->stream.get_executor(),
                                [self, result = std::move(result)]() mutable {
                                    if (self->serializer)
                                        self->send(result);
                                    else {
                                        const auto status = status_for(result);
                                        self->reply(status, std::move(result));
                                    }
                                });
                } catch (...) {
                    self->cancel();
                }
            } catch (...) {
                self->cancel();
            }
        });
    }
    void monitor_disconnect() {
        if (closed || monitoring)
            return;
        const auto limit = owner->server.options().limits.stream_queue_bytes;
        // Bound bytes pipelined while the current handler is still active.
        // Reading consumes those bytes, avoiding a perpetually readable peek.
        if (buffer.size() >= limit)
            return close();
        auto self = this->shared_from_this();
        try {
            auto incoming = buffer.prepare(std::min<std::size_t>(4096, limit - buffer.size()));
            monitoring = true;
            // TLS reads process encrypted close_notify; both transports retain
            // early HTTP bytes and keep watching until reply, stream or close.
            stream.async_read_some(
                incoming,
                net::bind_cancellation_slot(
                    monitor_cancel.slot(),
                    [self](boost::system::error_code error, std::size_t count) {
                        self->monitoring = false;
                        self->buffer.commit(count);
                        if (self->closed)
                            return;
                        if (self->serializer)
                            return self->close();
                        if (error && error != net::error::operation_aborted)
                            return self->close();
                        if (self->deferred_reply)
                            self->finish_deferred_reply();
                        else if (!error && count)
                            self->monitor_disconnect();
                        else if (!error)
                            self->close();
                    }));
        } catch (...) {
            monitoring = false;
            close();
        }
    }
    void open_stream() {
        stream_header = http::response<http::empty_body>(http::status::ok, 11);
        stream_header.set(http::field::content_type, "text/event-stream");
        stream_header.set(http::field::cache_control, "no-cache");
        stream_header.set("X-Accel-Buffering", "no");
        stream_header.chunked(true);
        stream_header.keep_alive(false);
        serializer.emplace(stream_header);
        auto self = this->shared_from_this();
        http::async_write_header(stream, *serializer,
                                 [self](boost::system::error_code error, std::size_t) {
                                     if (error)
                                         return self->close();
                                     self->header_written = true;
                                     self->write_next();
                                 });
        // The pending read becomes the SSE disconnect monitor. Start one only
        // when none remains, so TCP and SSL reads never overlap.
        if (!monitoring)
            monitor_disconnect();
    }
    void send(const Json &message) {
        if (closed.load() || overflow.load())
            return;
        JsonScope scope(owner->server.options().memory_resource);
        const bool result = message.contains("id") && !message.contains("method");
        std::string bytes = chunk("data: " + serialize_json(message) + "\n\n");
        const auto limit = owner->server.options().limits.stream_queue_bytes;
        if (result)
            bytes += "0\r\n\r\n";
        auto used = reserved.load();
        do {
            if (used > limit || bytes.size() > limit - used) {
                if (!overflow.exchange(true))
                    cancel();
                return;
            }
        } while (!reserved.compare_exchange_weak(used, used + bytes.size()));
        auto self = this->shared_from_this();
        owner->post(stream.get_executor(),
                  [self, bytes = String(bytes.data(), bytes.size()), result]() mutable {
                      if (self->closed || self->final) {
                          self->reserved.fetch_sub(bytes.size());
                          return;
                      }
                      if (!self->serializer)
                          self->open_stream();
                      self->enqueue(std::move(bytes), result);
                  });
    }
    void enqueue(String bytes, bool result) {
        queued += bytes.size();
        writes.push_back(std::move(bytes));
        if (result)
            final = true;
        write_next();
    }
    void write_next() {
        if (closed || !header_written || writing || writes.empty())
            return;
        writing = true;
        beast::get_lowest_layer(stream).expires_after(owner->options.idle_timeout);
        auto self = this->shared_from_this();
        net::async_write(stream, net::buffer(writes.front().data(), writes.front().size()),
                         [self](boost::system::error_code error, std::size_t) {
                             self->writing = false;
                             if (error)
                                 return self->close();
                             self->queued -= self->writes.front().size();
                             self->reserved.fetch_sub(self->writes.front().size());
                             self->writes.pop_front();
                             if (self->final && self->writes.empty())
                                 self->close();
                             else
                                 self->write_next();
                         });
    }
};
void HttpServer::Impl::accept() {
    if (stopping.load())
        return;
    auto self = shared_from_this();
    acceptor.async_accept(
        net::make_strand(io), [self](boost::system::error_code error, tcp::socket socket) {
            if (!error && !self->stopping.load()) {
                try {
                    JsonScope scope(self->server.options().memory_resource);
                    std::uint64_t key = 0;
                    {
                        std::lock_guard lock(self->channels_mutex);
                        if (self->channels.size() < self->options.max_connections)
                            key = self->next_channel++;
                    }
                    if (key) {
                        socket.set_option(tcp::no_delay(true));
                        socket.non_blocking(true);
                        std::shared_ptr<Channel> channel;
                        if (self->options.certificate_file.empty()) {
                            auto session = std::make_shared<Session<beast::tcp_stream>>(
                                self, beast::tcp_stream(std::move(socket)), key);
                            channel = session;
                            {
                                std::lock_guard lock(self->channels_mutex);
                                self->channels.emplace(key, channel);
                            }
                            session->start();
                        } else {
                            auto session =
                                std::make_shared<Session<beast::ssl_stream<beast::tcp_stream>>>(
                                    self,
                                    beast::ssl_stream<beast::tcp_stream>(
                                        beast::tcp_stream(std::move(socket)), self->tls),
                                    key);
                            channel = session;
                            {
                                std::lock_guard lock(self->channels_mutex);
                                self->channels.emplace(key, channel);
                            }
                            session->start();
                        }
                    }
                } catch (...) {
                }
            }
            if (!self->stopping.load())
                self->accept();
        });
}
HttpServer::HttpServer(Server &server, HttpServerOptions options)
    : impl_(std::make_shared<Impl>(server, std::move(options))) {}
HttpServer::~HttpServer() {
    stop();
}
void HttpServer::start() {
    auto impl = impl_;
    std::lock_guard lock(impl->lifecycle);
    if (impl->started || impl->stopping.load())
        throw std::logic_error("HTTP listener can be started once");
    const auto address = net::ip::make_address(impl->options.host);
    const tcp::endpoint endpoint(address, impl->options.port);
    impl->acceptor.open(endpoint.protocol());
    impl->acceptor.set_option(tcp::acceptor::reuse_address(true));
    impl->acceptor.bind(endpoint);
    impl->acceptor.listen();
    impl->bound_port.store(impl->acceptor.local_endpoint().port());
    impl->started = true;
    impl->accept();
    try {
        impl->threads.reserve(impl->options.io_threads);
        for (std::size_t i = 0; i < impl->options.io_threads; ++i)
            impl->threads.emplace_back([impl] {
                ListenerScope listener_scope(impl.get());
                JsonScope scope(impl->server.options().memory_resource);
                for (;;) {
                    try {
                        impl->io.run();
                        break;
                    } catch (...) {
                        impl->request_shutdown();
                        break;
                    }
                }
            });
        impl->shutdown.context = impl.get();
        impl->shutdown.request_stop = [](void *context) noexcept {
            static_cast<Impl *>(context)->request_shutdown();
        };
        impl->shutdown.thread = std::thread([impl] {
            impl->start_ready.wait(false);
            if (impl->start_failed.load())
                return;
            impl->shutdown_requested.wait(false);
            impl->finish_shutdown();
            // Always retire an unclaimed coordinator, even while the wrapper
            // remains alive. A completed registered thread cannot react to a
            // later reaper shutdown during static destruction.
            auto expected = detail::HttpShutdownThread::JoinOwner::none;
            if (impl->shutdown.join_owner.compare_exchange_strong(
                    expected, detail::HttpShutdownThread::JoinOwner::reaper))
                impl->reaper->retire(impl->shutdown, impl);
        });
        impl->coordinator_started = true;
        // Register once thread creation can no longer leave a partially
        // started listener coordinator idle. JSON retains its resource owner.
        impl->reaper = &detail::http_shutdown_reaper();
        impl->reaper->add(impl->shutdown);
        impl->start_ready.store(true);
        impl->start_ready.notify_all();
    } catch (...) {
        impl->request_shutdown();
        impl->start_failed.store(true);
        impl->start_ready.store(true);
        impl->start_ready.notify_all();
        if (impl->shutdown.thread.joinable())
            impl->shutdown.thread.join();
        impl->coordinator_started = false;
        impl->finish_shutdown();
        impl->shutdown.joined.store(true);
        impl->shutdown.joined.notify_all();
        throw;
    }
}
void HttpServer::stop() noexcept {
    auto impl = impl_;
    if (active_listener == impl.get() || detail::active_http_shutdown_reaper) {
        impl->request_shutdown();
        return;
    }
    std::lock_guard lock(impl->lifecycle);
    if (impl->shutdown.joined.load())
        return;
    if (!impl->coordinator_started) {
        impl->request_shutdown();
        impl->finish_shutdown();
        impl->shutdown.joined.store(true);
        impl->shutdown.joined.notify_all();
        return;
    }
    auto expected = detail::HttpShutdownThread::JoinOwner::none;
    const bool caller_joins = impl->shutdown.join_owner.compare_exchange_strong(
        expected, detail::HttpShutdownThread::JoinOwner::caller);
    impl->request_shutdown();
    if (caller_joins) {
        impl->shutdown_complete.wait(false);
        impl->shutdown.thread.join();
        impl->reaper->remove(impl->shutdown);
    } else
        impl->shutdown.joined.wait(false);
}
std::uint16_t HttpServer::port() const {
    return impl_->bound_port.load();
}
} // namespace mcp
