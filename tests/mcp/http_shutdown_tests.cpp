#include "http_headers.h"
#include "allocation_fault.h"
#include "http_channels.h"
#include "http_shutdown.h"
#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <thread>
#include <mcp/mcp.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#include <dbghelp.h>
#endif

using namespace mcp;
namespace {
namespace net = boost::asio;
namespace http = boost::beast::http;
Json expected_text_reply(const Server& server, int id, std::string_view text) {
  auto result = text_result(std::string(text));
  result["resultType"] = "complete";
  result["_meta"] = {{std::string(server_info_key), server.options().info}};
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}
struct ShutdownPeer {
  net::io_context io;
  net::ip::tcp::socket socket{io};
  boost::beast::flat_buffer buffer;
  std::uint16_t port;
  explicit ShutdownPeer(std::uint16_t value) : port(value) { socket.connect({net::ip::address_v4::loopback(), port}); }
  void send(Json request, bool stop_gate = false) {
    http::request<http::string_body> message(http::verb::post, "/mcp", 11);
    message.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    message.set(http::field::content_type, "application/json");
    message.set(http::field::accept, "application/json, text/event-stream");
    if (stop_gate)
      message.set("X-Stop-Gate", "true");
    for (const auto& [name, value] : detail::request_headers(request))
      message.set(name, value);
    message.body() = serialize_json(request);
    message.prepare_payload();
    http::write(socket, message);
  }
  void discover() {
    send(make_request(1, "server/discover"));
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    EXPECT_EQ(response.result_int(), 200U);
  }
  void expect_closed(std::optional<Json> expected_reply = {}) {
    char bytes[4096];
    std::string received;
    constexpr std::size_t limit = 64 * 1024;
    boost::system::error_code error;
    socket.non_blocking(true, error);
    ASSERT_FALSE(error) << error.message();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
      ASSERT_LT(received.size(), limit) << "Buffered response exceeds the closure read limit";
      const auto count = socket.read_some(net::buffer(bytes, std::min(sizeof(bytes), limit - received.size())), error);
      received.append(bytes, count);
      if (error && error != net::error::would_block && error != net::error::try_again)
        break;
      if (!count)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    EXPECT_TRUE(error == net::error::eof || error == net::error::connection_reset || error == net::error::operation_aborted) << error.message();
    if (received.empty())
      return;
    // A handler can finish its response before the asynchronous coordinator
    // closes the socket. Only an explicitly expected complete reply may
    // precede EOF; idle, rejected and cancelled peers remain byte-strict.
    ASSERT_TRUE(expected_reply.has_value()) << "Unexpected bytes before connection closure";
    http::response_parser<http::string_body> response;
    response.eager(true);
    const auto consumed = response.put(net::buffer(received), error);
    ASSERT_FALSE(error) << error.message();
    ASSERT_TRUE(response.is_done()) << "Buffered reply must be a complete HTTP response";
    ASSERT_EQ(consumed, received.size()) << "Extra bytes follow the expected reply";
    ASSERT_EQ(response.get().result_int(), 200U);
    ASSERT_EQ(response.get()[http::field::content_type], "application/json");
    Json message;
    ASSERT_NO_THROW(message = parse_json(response.get().body()));
    EXPECT_EQ(message, *expected_reply);
  }
};
void stop_with_watchdog(HttpServer& listener) {
  std::atomic<bool> finished{false};
  std::thread stopper([&] {
    listener.stop();
    finished.store(true);
  });
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!finished.load() && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (!finished.load()) {
    // Each discovered GTest is a process. Preserve a real failing exit
    // instead of hanging on the still-blocked stopper's destructor.
    std::fputs("HTTP stop did not complete within the controlled watchdog\n", stderr);
    std::fflush(stderr);
    std::_Exit(42);
  }
  stopper.join();
}
bool wait_expired(const std::weak_ptr<int>& sentinel) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!sentinel.expired() && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return sentinel.expired();
}

#if defined(_MSC_VER) && defined(_DEBUG)
using mcp_test::AllocationFault;
#endif
}  // namespace

TEST(Http, StopBeforeStartDoesNotCreateListenerThreads) {
  Server server;
  HttpServer listener(server);
  stop_with_watchdog(listener);
  EXPECT_THROW(listener.start(), std::logic_error);
  listener.stop();
}

TEST(Http, StopClosesKeepalivePeersAndCancelsCooperativeWorkerBeforeJoin) {
  Server server;
  HttpServer listener(server);
  std::promise<void> entered;
  std::atomic<bool> cancelled{false};
  server.add_tool({"wait"}, [&](const Json&, RequestContext& context) {
    std::stop_callback reentrant(context.stop_token(), [&] { listener.stop(); });
    entered.set_value();
    while (!context.stop_token().stop_requested())
      std::this_thread::yield();
    cancelled.store(true);
    return text_result("cancelled");
  });
  listener.start();
  ShutdownPeer idle(listener.port()), active(listener.port());
  idle.discover();
  active.send(make_request(2, "tools/call", {{"name", "wait"}, {"arguments", Json::object()}}));
  if (entered.get_future().wait_for(std::chrono::seconds(2)) != std::future_status::ready)
    std::_Exit(43);
  stop_with_watchdog(listener);
  EXPECT_TRUE(cancelled.load());
  idle.expect_closed();
  active.expect_closed();
}

TEST(Http, LastListenerOwnerReleasedFromWorkerIsJoinedByReaper) {
  Server server;  // The server retains its documented lifetime beyond the listener.
  auto sentinel = std::make_shared<int>(1);
  std::weak_ptr<int> weak = sentinel;
  HttpServerOptions options;
  options.authorize = [life = std::move(sentinel)](const Headers&) {
    return true;
  };
  auto listener = std::make_shared<HttpServer>(server, std::move(options));
  std::promise<void> owner_released;
  auto released = owner_released.get_future();
  server.add_tool({"release-owner"}, [&](const Json&, RequestContext&) {
    listener.reset();
    owner_released.set_value();
    return text_result("released");
  });
  listener->start();
  ShutdownPeer peer(listener->port());
  peer.send(make_request(1, "tools/call", {{"name", "release-owner"}, {"arguments", Json::object()}}));
  if (released.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
    std::_Exit(43);
  if (!wait_expired(weak))
    std::_Exit(42);
  peer.expect_closed(expected_text_reply(server, 1, "released"));
}

TEST(Http, LastListenerOwnerReleasedFromIoCallbackIsJoinedByReaper) {
  Server server;
  auto sentinel = std::make_shared<int>(1);
  std::weak_ptr<int> weak = sentinel;
  std::shared_ptr<HttpServer> listener;
  std::promise<void> owner_released;
  auto released = owner_released.get_future();
  HttpServerOptions options;
  options.authorize = [&, life = std::move(sentinel)](const Headers&) {
    listener.reset();
    owner_released.set_value();
    return true;
  };
  listener = std::make_shared<HttpServer>(server, std::move(options));
  listener->start();
  ShutdownPeer peer(listener->port());
  peer.send(make_request(1, "server/discover"));
  if (released.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
    std::_Exit(43);
  if (!wait_expired(weak))
    std::_Exit(42);
  peer.expect_closed();
}

TEST(Http, ReaperCanReleaseLastOwnerOfAnotherRetiredListener) {
  Server first_server, second_server;
  auto sentinel = std::make_shared<int>(1);
  std::weak_ptr<int> second_impl = sentinel;
  HttpServerOptions second_options;
  second_options.authorize = [life = std::move(sentinel)](const Headers&) {
    return true;
  };
  auto second = std::make_shared<HttpServer>(second_server, std::move(second_options));
  auto* second_listener = second.get();
  std::promise<void> second_stopped;
  second_server.add_tool({"stop"}, [&](const Json&, RequestContext&) {
    second_listener->stop();
    second_stopped.set_value();
    return text_result("stopped");
  });
  second->start();
  ShutdownPeer second_peer(second->port());
  second_peer.discover();

  std::promise<void> recycling, release;
  std::atomic<bool> released{false};
  struct CapturedListener {
    std::shared_ptr<HttpServer> listener;
    std::promise<void>& entered;
    std::shared_future<void> release;
    std::atomic<bool>& released;
    CapturedListener(std::shared_ptr<HttpServer> value, std::promise<void>& started, std::shared_future<void> gate, std::atomic<bool>& finished)
        : listener(std::move(value)), entered(started), release(std::move(gate)), released(finished) {}
    ~CapturedListener() {
      entered.set_value();
      release.wait();
      listener.reset();
      released.store(true);
    }
  };
  auto capture = std::make_shared<CapturedListener>(std::move(second), recycling, release.get_future().share(), released);
  HttpServerOptions first_options;
  first_options.authorize = [capture](const Headers&) {
    return true;
  };
  capture.reset();
  auto first = std::make_shared<HttpServer>(first_server, std::move(first_options));
  first_server.add_tool({"release-owner"}, [&](const Json&, RequestContext&) {
    first.reset();
    return text_result("released");
  });
  first->start();
  ShutdownPeer first_peer(first->port());
  first_peer.send(make_request(1, "tools/call", {{"name", "release-owner"}, {"arguments", Json::object()}}));
  if (recycling.get_future().wait_for(std::chrono::seconds(2)) != std::future_status::ready)
    std::_Exit(43);
  // The only reaper is now releasing the first Impl. Retire the second
  // coordinator while that release is held, then destroy its last wrapper.
  second_peer.send(make_request(2, "tools/call", {{"name", "stop"}, {"arguments", Json::object()}}));
  if (second_stopped.get_future().wait_for(std::chrono::seconds(2)) != std::future_status::ready)
    std::_Exit(44);
  second_peer.expect_closed(expected_text_reply(second_server, 2, "stopped"));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  release.set_value();
  if (!wait_expired(second_impl)) {
    std::fputs("Reaper blocked while releasing a captured retired listener\n", stderr);
    std::fflush(stderr);
    std::_Exit(42);
  }
  EXPECT_TRUE(released.load());
  first_peer.expect_closed(expected_text_reply(first_server, 1, "released"));
}

TEST(Http, ReaperCanReleaseLastOwnerOfListenerBeforeStart) {
  Server first_server, second_server;
  std::atomic<unsigned> business_invocations{0};
  auto sentinel = std::make_shared<int>(1);
  std::weak_ptr<int> second_impl = sentinel;
  HttpServerOptions second_options;
  second_options.authorize = [life = std::move(sentinel)](const Headers&) {
    return true;
  };
  auto second = std::make_shared<HttpServer>(second_server, std::move(second_options));
  HttpServerOptions first_options;
  first_options.authorize = [second = std::move(second)](const Headers&) {
    return true;
  };
  auto first = std::make_shared<HttpServer>(first_server, std::move(first_options));
  first_server.add_tool({"release-owner"}, [&](const Json&, RequestContext&) {
    first.reset();
    business_invocations.fetch_add(1);
    return text_result("released");
  });
  first->start();
  ShutdownPeer peer(first->port());
  peer.send(make_request(1, "tools/call", {{"name", "release-owner"}, {"arguments", Json::object()}}));
  if (!wait_expired(second_impl))
    std::_Exit(42);
  peer.expect_closed(expected_text_reply(first_server, 1, "released"));
  if (business_invocations.load() != 1U)
    std::_Exit(43);
  EXPECT_FALSE(first);
}

TEST(Http, DrainingSuccessfulReadDoesNotAdmitNewWorkAfterStop) {
  Server server;
  auto sentinel = std::make_shared<int>(1);
  std::weak_ptr<int> weak = sentinel;
  std::promise<void> entered, release;
  auto released = release.get_future();
  std::atomic<bool> stopped{false};
  std::atomic<unsigned> admitted{0}, invoked{0};
  std::shared_ptr<HttpServer> listener;
  HttpServerOptions options;
  options.io_threads = 1;
  options.authorize = [&, life = std::move(sentinel)](const Headers& headers) {
    if (headers.contains("x-stop-gate")) {
      listener->stop();
      stopped.store(true);
      entered.set_value();
      EXPECT_EQ(released.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    } else if (stopped.load())
      admitted.fetch_add(1);
    return true;
  };
  listener = std::make_shared<HttpServer>(server, std::move(options));
  struct Cleanup {
    std::shared_ptr<HttpServer>& listener;
    std::promise<void>& release;
    ~Cleanup() {
      try {
        release.set_value();
      } catch (...) {}
      if (listener)
        stop_with_watchdog(*listener);
    }
  } cleanup{listener, release};
  server.add_tool({"late"}, [&](const Json&, RequestContext&) {
    invoked.fetch_add(1);
    return text_result("unexpected");
  });
  listener->start();
  ShutdownPeer gate(listener->port()), queued(listener->port());
  gate.discover();
  queued.discover();
  gate.send(make_request(2, "server/discover"), true);
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  // The only IO thread remains in the authorizer while the other pending
  // keepalive read completes in the socket layer. Shutdown drains it later.
  queued.send(make_request(3, "tools/call", {{"name", "late"}, {"arguments", Json::object()}}));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  release.set_value();
  stop_with_watchdog(*listener);
  gate.expect_closed();
  queued.expect_closed();
  EXPECT_EQ(admitted.load(), 0U);
  EXPECT_EQ(invoked.load(), 0U);
  listener.reset();
  if (!wait_expired(weak))
    std::_Exit(42);
}

TEST(Http, StaticListenerStoppedByHandlerDoesNotHangDuringProcessExit) {
  // This test runs in its own discovered CTest process. The reaper is
  // constructed after these objects and consequently destroyed before them.
  static Server server;
  static HttpServer listener(server);
  auto entered = std::make_shared<std::promise<void>>();
  auto notified = entered->get_future();
  server.add_tool({"stop"}, [entered](const Json&, RequestContext&) {
    listener.stop();
    entered->set_value();
    return text_result("stopped");
  });
  listener.start();
  ShutdownPeer peer(listener.port());
  peer.send(make_request(1, "tools/call", {{"name", "stop"}, {"arguments", Json::object()}}));
  ASSERT_EQ(notified.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  peer.expect_closed(expected_text_reply(server, 1, "stopped"));
  // Permit the finite completion queue to drain before static destruction;
  // do not call external stop(), which would claim and join the coordinator.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(Http, SnapshotAllocationFailureClosesAllKeepalivePeersAndReturns) {
#if defined(_MSC_VER) && defined(_DEBUG)
  Server server;
  HttpServer listener(server);
  std::promise<void> entered;
  std::atomic<bool> cancelled{false};
  struct Cleanup {
    HttpServer& listener;
    ~Cleanup() { stop_with_watchdog(listener); }
  } cleanup{listener};
  server.add_tool({"wait"}, [&](const Json&, RequestContext& context) {
    std::stop_callback reentrant(context.stop_token(), [&] { listener.stop(); });
    entered.set_value();
    while (!context.stop_token().stop_requested())
      std::this_thread::yield();
    cancelled.store(true);
    return text_result("cancelled");
  });
  listener.start();
  std::unique_ptr<ShutdownPeer> peers[4];
  for (auto& peer : peers) {
    peer = std::make_unique<ShutdownPeer>(listener.port());
    peer->discover();
  }
  peers[3]->send(make_request(2, "tools/call", {{"name", "wait"}, {"arguments", Json::object()}}));
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  AllocationFault fault("*snapshot_live_channels*", 4 * sizeof(std::shared_ptr<void>));
  ASSERT_TRUE(fault.available()) << "Debug symbols must resolve the actual snapshot allocation";
  fault.arm();
  stop_with_watchdog(listener);
  fault.disarm();
  EXPECT_TRUE(fault.rejected()) << "The native allocator must actually reject snapshot.reserve";
  EXPECT_TRUE(cancelled.load()) << "The allocation-free fallback must cancel before joining workers";
  for (auto& peer : peers)
    peer->expect_closed();
#else
  GTEST_SKIP() << "Native CRT allocation injection requires the MSVC Debug runtime";
#endif
}

TEST(Http, AllocationFreeCloseRetainsLastOwnerOutsideRegistryLock) {
  struct Channel {
    std::function<void()> destroyed;
    ~Channel() { destroyed(); }
  };
  std::mutex mutex;
  std::map<unsigned, std::weak_ptr<Channel>> registry;
  std::shared_ptr<Channel> owners[3];
  unsigned destroyed = 0, visited = 0;
  for (unsigned index = 0; index < 3; ++index) {
    owners[index] = std::make_shared<Channel>();
    owners[index]->destroyed = [&, index] {
      std::lock_guard lock(mutex);
      registry.erase(index);
      ++destroyed;
    };
    registry.emplace(index, owners[index]);
  }
  owners[1].reset();  // The cursor must tolerate an expired/erased middle entry.
  detail::visit_live_channels(mutex, registry, [&](const auto& channel) {
    std::lock_guard lock(mutex);  // Visitor also requires lock-free registry access.
    ++visited;
    for (auto& owner : owners)
      if (owner == channel)
        owner.reset();
  });
  EXPECT_EQ(visited, 2U);
  EXPECT_EQ(destroyed, 3U);
  EXPECT_TRUE(registry.empty());
}

TEST(Http, ShutdownReaperDrainsLiveCoordinatorBeforeDestruction) {
  struct Record {
    detail::HttpShutdownThread shutdown;
    std::atomic<bool> stopped{false};
  };
  auto record = std::make_shared<Record>();
  std::weak_ptr<Record> weak = record;
  {
    detail::HttpShutdownReaper reaper;
    record->shutdown.context = record.get();
    record->shutdown.request_stop = [](void* context) noexcept {
      auto& value = *static_cast<Record*>(context);
      value.stopped.store(true);
      value.stopped.notify_all();
    };
    reaper.add(record->shutdown);
    record->shutdown.thread = std::thread([owner = record, &reaper] {
      owner->stopped.wait(false);
      owner->shutdown.join_owner.store(detail::HttpShutdownThread::JoinOwner::reaper);
      reaper.retire(owner->shutdown, owner);
    });
    record.reset();
  }
  EXPECT_TRUE(weak.expired()) << "Reaper destruction must join before releasing the record";
}

TEST(Http, PostAllocationFailureRequestsShutdownWithoutTerminatingWorker) {
#if defined(_MSC_VER) && defined(_DEBUG)
  Server server;
  HttpServer listener(server);
  AllocationFault fault("*HttpServer::Impl::post*");
  ASSERT_TRUE(fault.available());
  std::promise<void> entered, release;
  auto released = release.get_future();
  struct Cleanup {
    HttpServer& listener;
    std::promise<void>& release;
    ~Cleanup() {
      try {
        release.set_value();
      } catch (...) {}
      stop_with_watchdog(listener);
    }
  } cleanup{listener, release};
  server.add_tool({"post-failure"}, [&](const Json&, RequestContext&) {
    entered.set_value();
    EXPECT_EQ(released.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    return text_result("done");
  });
  listener.start();
  ShutdownPeer peer(listener.port());
  peer.send(make_request(1, "tools/call", {{"name", "post-failure"}, {"arguments", Json::object()}}));
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  fault.arm();
  release.set_value();
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!fault.rejected() && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  fault.disarm();
  EXPECT_TRUE(fault.rejected());
  stop_with_watchdog(listener);
  peer.expect_closed();
#else
  GTEST_SKIP() << "Native CRT allocation injection requires the MSVC Debug runtime";
#endif
}

TEST(Http, PartialIoThreadCreationFailureJoinsAlreadyStartedThread) {
#if defined(_MSC_VER) && defined(_DEBUG)
  Server server;
  HttpServerOptions options;
  options.io_threads = 2;
  HttpServer listener(server, options);
  AllocationFault fault("*HttpServer::start*", sizeof(std::shared_ptr<void>), 2, GetCurrentThreadId());
  ASSERT_TRUE(fault.available());
  fault.arm();
  EXPECT_THROW(listener.start(), std::bad_alloc);
  fault.disarm();
  EXPECT_TRUE(fault.rejected()) << "The second native thread-capture allocation must fail";
  stop_with_watchdog(listener);
  EXPECT_THROW(listener.start(), std::logic_error);
#else
  GTEST_SKIP() << "Native CRT allocation injection requires the MSVC Debug runtime";
#endif
}
