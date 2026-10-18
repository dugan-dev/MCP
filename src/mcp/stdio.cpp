#include "deadline.h"
#include "notification.h"
#include "request_id.h"
#include "stdio_request_state.h"
#include "process_launch.h"
#include <boost/asio.hpp>
#include <mcp/stdio.h>
#include <mcp/memory.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <thread>

namespace mcp {
namespace net = boost::asio;
namespace bp = boost::process::v2;
namespace {
std::string id_key(const Json &id) {
    return detail::canonical_id(id);
}
bool line(std::istream &input, std::string &output, std::size_t limit, bool &oversized) {
    output.clear();
    oversized = false;
    bool read = false;
    char c;
    while (input.get(c)) {
        read = true;
        if (c == '\n')
            break;
        if (output.size() < limit)
            output += c;
        else
            oversized = true;
    }
    if (output.ends_with('\r'))
        output.pop_back();
    return read;
}
} // namespace
void run_stdio(Server &server, std::istream &input, std::ostream &output, StdioOptions options) {
    if (!options.worker_threads)
        throw std::invalid_argument("Stdio worker count must be positive");
    JsonScope scope(server.options().memory_resource);
    net::thread_pool workers(options.worker_threads);
    std::mutex mutex, output_mutex;
    std::condition_variable drained;
    using RequestState = std::shared_ptr<detail::StdioRequestState>;
    std::map<std::string, RequestState> pending;
    struct ActiveSubscription {
        RequestState state;
        std::shared_ptr<Subscription> handle;
    };
    std::map<std::string, ActiveSubscription> subscriptions;
    std::atomic<bool> failed{false};
    auto send = [&](const Json &message, const detail::StdioRequestState *state = nullptr) {
        if (message.is_null() || (state && state->is_cancelled()))
            return;
        auto bytes = serialize_json(message);
        std::lock_guard lock(output_mutex);
        if (state && state->is_cancelled())
            return;
        if (bytes.size() > server.options().limits.message_bytes)
            throw TransportError(TransportErrorKind::limit, "Stdio output exceeds byte limit");
        if (failed.load())
            throw TransportError(TransportErrorKind::closed, "Stdio output is closed");
        output << bytes << '\n';
        output.flush();
        if (!output) {
            failed.store(true);
            throw TransportError(TransportErrorKind::closed, "Cannot write stdio output");
        }
    };
    std::string bytes;
    bool oversized;
    while (!failed.load() && line(input, bytes, server.options().limits.message_bytes, oversized)) {
        Json request;
        try {
            if (oversized)
                throw RpcError(ErrorCode::parse_error, "Stdio line exceeds byte limit");
            request = parse_json(bytes, server.options().limits.message_bytes,
                                 server.options().limits.json_depth);
            if (request.is_object() && !request.contains("id") && request.contains("method")) {
                if (request["method"] == "notifications/cancelled" &&
                    request.value("jsonrpc", Json()) == "2.0" &&
                    !request.contains("result") && !request.contains("error") &&
                    request.contains("params") &&
                    request["params"].is_object() && request["params"].contains("requestId") &&
                    valid_id(request["params"]["requestId"]) &&
                    (!request["params"].contains("reason") || request["params"]["reason"].is_string())) {
                    const auto key = id_key(request["params"]["requestId"]);
                    RequestState state;
                    std::shared_ptr<Subscription> sub;
                    {
                        // Cancellation changes the admission marker and output
                        // boundary together. No callback runs under either lock.
                        std::scoped_lock lock(mutex, output_mutex);
                        if (auto it = pending.find(key); it != pending.end())
                            state = it->second;
                        if (auto it = subscriptions.find(key); it != subscriptions.end()) {
                            state = it->second.state;
                            sub = std::move(it->second.handle);
                            subscriptions.erase(it);
                        }
                        if (state)
                            state->mark_cancelled();
                    }
                    if (state)
                        state->request_stop();
                    if (sub)
                        sub->close();
                }
                continue;
            }
            validate_request(request);
        } catch (const RpcError &error) {
            Json id;
            if (request.is_object() && request.contains("id"))
                id = request["id"];
            try {
                send(make_error(id, error.code(), error.what(), error.data()));
            } catch (...) {
                failed.store(true);
            }
            continue;
        }
        const auto key = id_key(request["id"]);
        auto state = std::make_shared<detail::StdioRequestState>();
        bool accepted = false;
        {
            std::lock_guard lock(mutex);
            if (pending.size() + subscriptions.size() < server.options().limits.pending_requests &&
                !pending.contains(key) && !subscriptions.contains(key))
                accepted = pending.emplace(key, state).second;
        }
        if (!accepted) {
            try {
                send(make_error(request["id"], 1002,
                                "Duplicate request ID or stdio capacity exceeded"));
            } catch (...) {
                failed.store(true);
            }
            continue;
        }
        net::post(workers, [&, request = std::move(request), key, state]() mutable {
            JsonScope worker_scope(server.options().memory_resource);
            auto send_for_request = [&, state](const Json &message) {
                send(message, state.get());
            };
            try {
                if (request["method"] == "subscriptions/listen") {
                    if (state->can_activate()) {
                        auto subscription = server.subscribe(request, send_for_request);
                        bool cancel = false;
                        {
                            std::lock_guard lock(mutex);
                            cancel = !state->can_activate();
                            if (!cancel)
                                subscriptions.emplace(key, ActiveSubscription{state, subscription});
                        }
                        if (cancel)
                            subscription->close();
                    }
                } else if (!state->is_cancelled()) {
                    auto result = server.dispatch(request, state->stop_token(), send_for_request);
                    send_for_request(result);
                }
            } catch (const RpcError &error) {
                try {
                    send_for_request(make_error(request["id"], error.code(), error.what(), error.data()));
                } catch (...) {
                    failed.store(true);
                }
            } catch (...) {
                failed.store(true);
            }
            {
                std::lock_guard lock(mutex);
                pending.erase(key);
            }
            drained.notify_all();
        });
    }
    if (!options.drain_on_eof) {
        std::vector<RequestState> stopping;
        {
            std::scoped_lock lock(mutex, output_mutex);
            for (auto &[key, state] : pending) {
                (void)key;
                state->mark_cancelled();
                stopping.push_back(state);
            }
            for (auto &[key, subscription] : subscriptions) {
                (void)key;
                subscription.state->mark_cancelled();
                stopping.push_back(subscription.state);
            }
        }
        for (const auto &state : stopping)
            state->request_stop();
    }
    workers.join();
    for (auto &[key, subscription] : subscriptions) {
        (void)key;
        try {
            subscription.handle->close();
        } catch (...) {
        }
    }
}
struct StdioTransport::Impl : std::enable_shared_from_this<Impl> {
    struct Pending {
        std::mutex mutex;
        std::condition_variable_any ready;
        std::optional<Json> response;
        std::deque<Json> notifications;
        std::size_t notification_bytes = 0;
        std::exception_ptr error;
        NotificationHandler notify;
        Json request;
        detail::NotificationState notification_state;
    };
    ProcessOptions options;
    // Prepare the shutdown error before launching a child. close() and native
    // cancellation completions must not allocate just to report closure.
    const std::exception_ptr closed_error = std::make_exception_ptr(
        TransportError(TransportErrorKind::closed, "Subprocess transport closed"));
    net::io_context io;
    net::executor_work_guard<net::io_context::executor_type> guard{net::make_work_guard(io)};
    net::writable_pipe input{io};
    net::readable_pipe output{io};
    detail::ProcessTree tree;
    std::unique_ptr<bp::process> child;
    std::thread thread;
    std::mutex mutex, lifecycle;
    std::condition_variable_any log_available;
    std::map<std::string, std::shared_ptr<Pending>> pending;
    std::shared_ptr<Pending> logged;
    std::deque<String, JsonAllocator<String>> writes;
    std::atomic<std::size_t> queued{0};
    String read_buffer;
    std::array<char, 4096> block;
    std::atomic<bool> closed{false};
    bool joined = false;
    explicit Impl(ProcessOptions value) : options(std::move(value)) {
        if (options.executable.empty() || !options.limits.message_bytes ||
            !options.limits.pending_requests || options.shutdown_timeout.count() < 0)
            throw std::invalid_argument("Invalid subprocess options");
        detail::validate_process_arguments(options.arguments);
        const auto executable = detail::process_path(options.executable);
        auto environment = detail::process_environment(options.inherit_environment, options.environment);
        const auto directory = options.working_directory.empty()
            ? std::filesystem::current_path() : detail::process_path(options.working_directory);
#ifdef _WIN32
        child = std::make_unique<bp::process>(io, executable, options.arguments,
            bp::process_stdio{.in = input, .out = output}, environment,
            bp::process_start_dir{directory}, tree, bp::windows::show_window_hide);
#else
        child = std::make_unique<bp::process>(io, executable, options.arguments,
            bp::process_stdio{.in = input, .out = output}, environment,
            bp::process_start_dir{directory}, tree);
#endif
        tree.track(child->id());
        tree.check_launch();
    }
    void fail(std::exception_ptr error) {
        {
            std::lock_guard lock(mutex);
            // Serialize closure with send registration, so no owning handler
            // can be posted after close has finished draining the context.
            closed.store(true);
            for (auto &[key, p] : pending) {
                (void)key;
                // exchange releases p->mutex before its Cleanup acquires
                // mutex; notification callbacks also run without p->mutex.
                std::lock_guard pending_lock(p->mutex);
                p->error = error;
                p->ready.notify_all();
            }
        }
        log_available.notify_all();
    }
    void close_pipes() {
        boost::system::error_code error;
        input.close(error);
        output.close(error);
        guard.reset();
    }
    void schedule_close() {
        auto self = shared_from_this();
        net::post(io, [self] { self->close_pipes(); });
    }
    void read() {
        auto self = shared_from_this();
        output.async_read_some(net::buffer(block), [self](boost::system::error_code error,
                                                          std::size_t count) {
            JsonScope scope(self->options.memory_resource);
            if (error) {
                self->fail(self->closed_error);
                return;
            }
            try {
                for (std::size_t i = 0; i < count; ++i) {
                    if (self->block[i] == '\n') {
                        if (self->read_buffer.ends_with('\r'))
                            self->read_buffer.pop_back();
                        auto message = parse_json(
                            std::string_view(self->read_buffer.data(), self->read_buffer.size()),
                            self->options.limits.message_bytes, self->options.limits.json_depth);
                        self->read_buffer.clear();
                        self->message(std::move(message));
                    } else {
                        if (self->read_buffer.size() >= self->options.limits.message_bytes)
                            throw TransportError(TransportErrorKind::limit,
                                                 "Subprocess line exceeds byte limit");
                        self->read_buffer += self->block[i];
                    }
                }
                if (!self->closed.load())
                    self->read();
            } catch (const RpcError &) {
                self->fail(std::make_exception_ptr(TransportError(
                    TransportErrorKind::invalid_response, "Invalid subprocess JSON")));
            } catch (...) {
                self->fail(std::current_exception());
            }
        });
    }
    void message(Json value) {
        if (!value.is_object() || value.value("jsonrpc", Json()) != "2.0")
            throw TransportError(TransportErrorKind::invalid_response,
                                 "Invalid subprocess message");
        if (value.contains("method")) {
            if (value.contains("id") || !value["method"].is_string() || !value.contains("params") ||
                !value["params"].is_object())
                throw TransportError(TransportErrorKind::invalid_response,
                                     "Invalid subprocess notification");
            Json subscription_id =
                value["params"].contains("_meta") && value["params"]["_meta"].is_object()
                    ? value["params"]["_meta"].value(subscription_key, Json())
                    : Json();
            const Json progress_token = value["params"].value("progressToken", Json());
            std::vector<std::shared_ptr<Pending>> snapshot;
            {
                std::lock_guard lock(mutex);
                if (subscription_id.is_null() && progress_token.is_null() &&
                    value["method"] == "notifications/message" && logged)
                    snapshot.push_back(logged);
                for (auto &[key, p] : pending) {
                    (void)key;
                    if ((!subscription_id.is_null() && p->request["id"] == subscription_id) ||
                        (!progress_token.is_null() &&
                         p->request["params"]["_meta"].value("progressToken", Json()) ==
                             progress_token))
                        snapshot.push_back(p);
                }
            }
            for (const auto &p : snapshot) {
                const auto size = serialize_json(value).size();
                std::lock_guard lock(p->mutex);
                detail::validate_notification(value, p->request, p->notification_state);
                if (size > options.limits.stream_queue_bytes - p->notification_bytes)
                    p->error = std::make_exception_ptr(
                        TransportError(TransportErrorKind::limit,
                                       "Subprocess notification queue exceeds byte limit"));
                else {
                    p->notification_bytes += size;
                    p->notifications.push_back(value);
                }
                p->ready.notify_all();
            }
        } else {
            if (!value.contains("id") || !valid_id(value["id"]))
                throw TransportError(TransportErrorKind::invalid_response,
                                     "Subprocess response has no ID");
            std::shared_ptr<Pending> target;
            {
                std::lock_guard lock(mutex);
                const auto it = pending.find(id_key(value["id"]));
                if (it != pending.end())
                    target = it->second;
            }
            if (!target)
                return;
            std::lock_guard lock(target->mutex);
            detail::validate_stream_end(value, target->request, target->notification_state);
            if (target->response)
                throw TransportError(TransportErrorKind::invalid_response,
                                     "Duplicate subprocess response");
            target->response = std::move(value);
            target->ready.notify_all();
        }
    }
    void write_next() {
        if (writes.empty() || closed.load())
            return;
        auto self = shared_from_this();
        net::async_write(input, net::buffer(writes.front().data(), writes.front().size()),
                         [self](boost::system::error_code error, std::size_t) {
                             if (error) {
                                 self->fail(self->closed_error);
                                 return;
                             }
                             self->queued -= self->writes.front().size();
                             self->writes.pop_front();
                             self->write_next();
                         });
    }
    void send(Json value) {
        auto bytes = serialize_json(value);
        if (bytes.size() >= options.limits.message_bytes)
            throw TransportError(TransportErrorKind::limit,
                                 "Subprocess request exceeds byte limit");
        bytes += '\n';
        const auto count = bytes.size();
        auto self = shared_from_this();
        std::lock_guard lock(mutex);
        if (closed.load())
            std::rethrow_exception(closed_error);
        auto reserved = queued.load(std::memory_order_relaxed);
        do {
            if (reserved > options.limits.stream_queue_bytes ||
                bytes.size() > options.limits.stream_queue_bytes - reserved)
                throw TransportError(TransportErrorKind::limit, "Subprocess write queue exceeds byte limit");
        } while (!queued.compare_exchange_weak(reserved, reserved + bytes.size()));
        try {
            net::post(io, [self, bytes = std::move(bytes)] {
                if (self->closed.load()) {
                    self->queued -= bytes.size();
                    return;
                }
                const bool idle = self->writes.empty();
                self->writes.emplace_back(bytes.data(), bytes.size());
                if (idle)
                    self->write_next();
            });
        } catch (...) {
            queued -= count;
            throw;
        }
    }
};
StdioTransport::StdioTransport(ProcessOptions options) {
    if (!options.memory_resource)
        options.memory_resource = memory::default_resource();
    JsonScope scope(options.memory_resource);
    impl_ = std::make_shared<Impl>(std::move(options));
    auto impl = impl_;
    impl->thread = std::thread([impl] {
        JsonScope scope(impl->options.memory_resource);
        try {
            // Register self-owning asynchronous work only after thread creation
            // succeeds; otherwise constructor unwinding cannot release Impl.
            impl->read();
            impl->io.run();
        } catch (...) {
            impl->fail(std::current_exception());
        }
    });
}
StdioTransport::~StdioTransport() {
    close();
}
Json StdioTransport::exchange(const Json &request, const RequestOptions &options,
                              NotificationHandler notify) {
    auto impl = impl_;
    JsonScope scope(impl->options.memory_resource);
    validate_request(request);
    if (options.timeout.count() <= 0)
        throw std::invalid_argument("Timeout must be positive");
    const auto deadline = detail::request_deadline(options.timeout);
    const bool logging = request["method"] != "subscriptions/listen" &&
        request["params"]["_meta"].contains("io.modelcontextprotocol/logLevel");
    const auto key = id_key(request["id"]);
    auto p = std::make_shared<Impl::Pending>();
    p->notify = std::move(notify);
    p->request = request;
    {
        std::unique_lock lock(impl->mutex);
        if (logging && !impl->log_available.wait_until(lock, options.stop, deadline, [&] {
                return impl->closed.load() || !impl->logged;
            }))
            throw TransportError(options.stop.stop_requested() ? TransportErrorKind::cancelled
                                                               : TransportErrorKind::timeout,
                                 "Subprocess logging slot wait abandoned");
        if (options.stop.stop_requested())
            throw TransportError(TransportErrorKind::cancelled, "Subprocess request cancelled");
        if (impl->closed.load())
            throw TransportError(TransportErrorKind::closed, "Subprocess transport is closed");
        if (std::chrono::steady_clock::now() >= deadline)
            throw TransportError(TransportErrorKind::timeout, "Subprocess request deadline exceeded");
        if (impl->pending.size() >= impl->options.limits.pending_requests ||
            !impl->pending.emplace(key, p).second)
            throw TransportError(TransportErrorKind::limit,
                                 "Subprocess pending limit or duplicate ID");
        if (logging)
            impl->logged = p;
    }
    struct Cleanup {
        StdioTransport *transport;
        std::shared_ptr<Impl> impl;
        std::string key;
        bool logging, sent = false, completed = false;
        ~Cleanup() {
            // A late, untagged log cannot be correlated after abandoning its request.
            // Close before releasing the slot so a later request never receives it.
            if (logging && sent && !completed)
                transport->close();
            {
                std::lock_guard lock(impl->mutex);
                impl->pending.erase(key);
                if (logging)
                    impl->logged.reset();
            }
            if (logging)
                impl->log_available.notify_all();
        }
    } cleanup{this, impl, key, logging};
    impl->send(request);
    cleanup.sent = true;
    std::unique_lock lock(p->mutex);
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            lock.unlock();
            try {
                impl->send(make_notification("notifications/cancelled",
                                             {{"requestId", request["id"]}}));
            } catch (...) {
            }
            throw TransportError(TransportErrorKind::timeout,
                                 "Subprocess callback exceeded request deadline");
        }
        const bool ready = p->ready.wait_until(lock, options.stop, deadline, [&] {
            return p->error || p->response || !p->notifications.empty();
        });
        if (p->error)
            std::rethrow_exception(p->error);
        if (!ready || options.stop.stop_requested()) {
            lock.unlock();
            try {
                impl->send(
                    make_notification("notifications/cancelled", {{"requestId", request["id"]}}));
            } catch (...) {
            }
            throw TransportError(options.stop.stop_requested() ? TransportErrorKind::cancelled
                                                               : TransportErrorKind::timeout,
                                 "Subprocess request abandoned");
        }
        if (!p->notifications.empty()) {
            auto message = std::move(p->notifications.front());
            p->notifications.pop_front();
            p->notification_bytes -= serialize_json(message).size();
            lock.unlock();
            try {
                if (p->notify)
                    p->notify(message);
            } catch (...) {
                try {
                    impl->send(make_notification("notifications/cancelled",
                                                 {{"requestId", request["id"]}}));
                } catch (...) {
                }
                throw;
            }
            lock.lock();
        } else {
            cleanup.completed = true;
            return std::move(*p->response);
        }
    }
}
void StdioTransport::close() noexcept {
    auto impl = impl_;
    std::lock_guard lock(impl->lifecycle);
    if (impl->joined)
        return;
    impl->fail(impl->closed_error);
    try {
        impl->schedule_close();
    } catch (...) {
        // No queued shutdown owns Impl when post cannot allocate. Stop the
        // worker before touching its pipes from this thread.
        impl->io.stop();
    }
    if (impl->thread.joinable() && std::this_thread::get_id() != impl->thread.get_id())
        impl->thread.join();
    else if (impl->thread.joinable()) {
        if (impl->io.stopped())
            impl->close_pipes(); // Already on the owning I/O thread.
        return;
    }
    impl->close_pipes();
    // A worker may have exited on an exception before processing the shutdown
    // post. Drain its cancelled operations and queued self-owning closures.
    impl->io.restart();
    for (;;) {
        try {
            impl->io.run();
            break;
        } catch (...) {
            impl->fail(impl->closed_error);
        }
    }
    try {
        const auto deadline = detail::request_deadline(impl->options.shutdown_timeout);
        while (impl->child->running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        impl->tree.terminate();
        boost::system::error_code error;
        if (impl->child->running(error)) impl->child->terminate(error);
        impl->child->wait(error);
    } catch (...) {
        impl->tree.terminate();
    }
    impl->joined = true;
}
} // namespace mcp
