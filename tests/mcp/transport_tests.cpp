#include "http_headers.h"
#include "allocation_fault.h"
#include "http_channels.h"
#include "notification.h"
#include "stdio_request_state.h"
#ifndef _WIN32
#include "process_launch.h"
#endif
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <climits>
#include <curl/curl.h>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <set>
#include <system_error>
#include <sstream>
#include <thread>
#include <utility>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <cerrno>
#include <unistd.h>
#endif
#include <mcp/mcp.h>

using namespace mcp;
namespace {
struct Harness {
  Server server;
  HttpServer listener;
  Harness(HttpServerOptions options = {}) : listener(server, std::move(options)) {
    server.add_tool({"echo", "", {{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}, {"required", {"message"}}}},
                    [](const Json& args, RequestContext&) { return text_result(text(args["message"])); });
    listener.start();
  }
  std::string url(std::string_view scheme = "http") const { return std::string(scheme) + "://127.0.0.1:" + std::to_string(listener.port()) + "/mcp"; }
};
template <class Stream>
void send_http_request(Stream& stream, std::uint16_t port, const Json& request) {
  namespace http = boost::beast::http;
  http::request<http::string_body> message(http::verb::post, "/mcp", 11);
  message.set(http::field::host, "127.0.0.1:" + std::to_string(port));
  message.set(http::field::content_type, "application/json");
  message.set(http::field::accept, "application/json, text/event-stream");
  for (const auto& [name, value] : detail::request_headers(request))
    message.set(name, value);
  message.body() = serialize_json(request);
  message.prepare_payload();
  http::write(stream, message);
}
template <class Stream>
Json receive_http_response(Stream& stream, boost::beast::flat_buffer& buffer) {
  boost::beast::http::response<boost::beast::http::string_body> response;
  boost::beast::http::read(stream, buffer, response);
  EXPECT_EQ(response.result_int(), 200U);
  return parse_json(response.body());
}
struct PlainPeer {
  boost::asio::io_context io;
  boost::asio::ip::tcp::socket stream{io};
  boost::beast::flat_buffer buffer;
  std::uint16_t port;
  explicit PlainPeer(std::uint16_t value) : port(value) { stream.connect({boost::asio::ip::address_v4::loopback(), port}); }
  void send(const Json& request) { send_http_request(stream, port, request); }
  Json receive() { return receive_http_response(stream, buffer); }
  void disconnect() {
    boost::system::error_code error;
    stream.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream.close(error);
  }
};
struct TlsPeer {
  boost::asio::io_context io;
  static boost::asio::ssl::context context() {
    boost::asio::ssl::context value(boost::asio::ssl::context::tls_client);
    value.load_verify_file(std::string(MCP_FIXTURES) + "/localhost.crt");
    value.set_verify_mode(boost::asio::ssl::verify_peer);
    return value;
  }
  boost::asio::ssl::context tls = context();
  boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream{io, tls};
  boost::beast::flat_buffer buffer;
  std::uint16_t port;
  explicit TlsPeer(std::uint16_t value) : port(value) {
    stream.lowest_layer().connect({boost::asio::ip::address_v4::loopback(), port});
    stream.handshake(boost::asio::ssl::stream_base::client);
  }
  void send(const Json& request) { send_http_request(stream, port, request); }
  Json receive() { return receive_http_response(stream, buffer); }
  void disconnect() {
    boost::system::error_code error;
    stream.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream.lowest_layer().close(error);
  }
};
struct Raw {
  long status;
  std::string body;
};
Raw post(std::string_view url, std::string body, Headers headers, const char* method = "POST") {
  CURL* easy = curl_easy_init();
  if (!easy)
    throw std::bad_alloc();
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> owner(easy, curl_easy_cleanup);
  std::string address(url);
  Raw result{};
  curl_slist* fields = nullptr;
  for (const auto& [name, value] : headers)
    fields = curl_slist_append(fields, (name + ": " + value).c_str());
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> field_owner(fields, curl_slist_free_all);
  curl_easy_setopt(easy, CURLOPT_URL, address.c_str());
  curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, fields);
  curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 5000L);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(
      easy, CURLOPT_WRITEFUNCTION, +[](char* data, std::size_t size, std::size_t count, void* p) -> std::size_t {
        static_cast<std::string*>(p)->append(data, size * count);
        return size * count;
      });
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &result.body);
  const auto code = curl_easy_perform(easy);
  if (code != CURLE_OK)
    throw std::runtime_error(curl_easy_strerror(code));
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status);
  return result;
}
Headers headers(const Json& request) {
  auto result = detail::request_headers(request);
  result["accept"] = "application/json, text/event-stream";
  result["content-type"] = "application/json";
  return result;
}
struct HttpPeer {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor{io, {boost::asio::ip::tcp::v4(), 0}};
  std::future<void> worker;
  HttpPeer(unsigned status, std::string type, std::string body, bool interim = false) {
    worker = std::async(std::launch::async, [this, status, type, body, interim] {
      namespace http = boost::beast::http;
      boost::asio::ip::tcp::socket socket(io);
      acceptor.accept(socket);
      boost::beast::flat_buffer buffer;
      http::request<http::string_body> request;
      http::read(socket, buffer, request);
      boost::system::error_code error;
      if (interim)
        boost::asio::write(socket, boost::asio::buffer(std::string("HTTP/1.1 103 Early Hints\r\nContent-Type: text/event-stream\r\n\r\n")), error);
      http::response<http::string_body> response(static_cast<http::status>(status), 11);
      response.set(http::field::content_type, type);
      response.body() = body;
      response.keep_alive(false);
      response.prepare_payload();
      http::write(socket, response, error);
    });
  }
  ~HttpPeer() { worker.wait(); }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/mcp"; }
};
ProcessOptions logging_peer() {
  ProcessOptions options;
  options.executable = MCP_TEST_PEER;
  options.arguments = {"logging"};
  options.shutdown_timeout = std::chrono::milliseconds(20);
  return options;
}
class EofSignalBuffer final : public std::stringbuf {
 public:
  EofSignalBuffer(std::string bytes, std::function<void()> signal) : std::stringbuf(std::move(bytes), std::ios::in), signal_(std::move(signal)) {}

 private:
  std::function<void()> signal_;
  bool signalled_ = false;
  int_type underflow() override {
    const auto value = std::stringbuf::underflow();
    if (traits_type::eq_int_type(value, traits_type::eof()) && !signalled_) {
      signalled_ = true;
      signal_();
    }
    return value;
  }
};
class GatedInputBuffer final : public std::stringbuf {
 public:
  GatedInputBuffer(std::string head, std::string tail, std::function<void()> gate, std::function<void()> eof = {})
      : std::stringbuf(std::move(head), std::ios::in), tail_(std::move(tail)), gate_(std::move(gate)), eof_(std::move(eof)) {}

 private:
  std::string tail_;
  std::function<void()> gate_;
  std::function<void()> eof_;
  bool opened_ = false, finished_ = false;
  int_type underflow() override {
    auto value = std::stringbuf::underflow();
    if (traits_type::eq_int_type(value, traits_type::eof()) && !opened_) {
      opened_ = true;
      gate_();
      str(std::move(tail_));
      value = std::stringbuf::underflow();
    }
    if (traits_type::eq_int_type(value, traits_type::eof()) && opened_ && !finished_) {
      finished_ = true;
      if (eof_)
        eof_();
    }
    return value;
  }
};
class ResponseSignalBuffer final : public std::stringbuf {
 public:
  explicit ResponseSignalBuffer(std::function<void()> signal) : signal_(std::move(signal)) {}

 private:
  std::function<void()> signal_;
  bool signalled_ = false;
  int sync() override {
    if (!signalled_ && str().find("\"id\":2") != std::string::npos) {
      signalled_ = true;
      signal_();
    }
    return std::stringbuf::sync();
  }
};
std::vector<Json> stdio_messages(const std::string& bytes) {
  std::vector<Json> result;
  std::istringstream input(bytes);
  for (std::string line; std::getline(input, line);)
    result.push_back(parse_json(line));
  return result;
}
}  // namespace

TEST(Http, ResponseMediaTypeCannotUsePrefixAliases) {
  const auto request = make_request(71, "tools/call", {{"name", "work"}, {"_meta", {{"io.modelcontextprotocol/logLevel", "info"}}}});
  const auto result = serialize_json(Json{{"jsonrpc", "2.0"}, {"id", 71}, {"result", text_result("ok")}});
  const auto notice = serialize_json(make_notification("notifications/message", {{"level", "info"}, {"data", "must not be delivered"}}));
  for (const auto& type : {"application/jsonx", "application/json+xml", "text/event-streamx", "text/event-stream+xml"}) {
    SCOPED_TRACE(type);
    const bool sse = std::string_view(type).starts_with("text/");
    HttpPeer peer(200, type, sse ? "data: " + notice + "\n\ndata: " + result + "\n\n" : result);
    HttpTransport transport(HttpClientOptions{peer.url()});
    int callbacks = 0;
    try {
      (void)transport.exchange(request, {}, [&](const Json&) { ++callbacks; });
      ADD_FAILURE() << "An unsupported media type must be rejected";
    } catch (const TransportError& error) {
      EXPECT_EQ(error.kind(), TransportErrorKind::invalid_response);
    }
    EXPECT_EQ(callbacks, 0);
  }
}

TEST(Http, ResponseMediaTypeAllowsCaseAndParameters) {
  const auto request = make_request(71, "server/discover");
  const auto result = serialize_json(Json{{"jsonrpc", "2.0"}, {"id", 71}, {"result", {{"resultType", "complete"}}}});
  for (const auto& type : {"Application/JSON; charset=utf-8", "Text/Event-Stream; charset=utf-8"}) {
    SCOPED_TRACE(type);
    const bool sse = std::string_view(type).starts_with("Text/");
    HttpPeer peer(200, type, sse ? "data: " + result + "\n\n" : result);
    HttpTransport transport(HttpClientOptions{peer.url()});
    EXPECT_EQ(transport.exchange(request, {})["id"], 71);
  }
}

TEST(Stdio, CancelledNotificationRequiresValidJsonRpcEnvelope) {
  for (int mutation = 0; mutation < 5; ++mutation) {
    SCOPED_TRACE(mutation);
    Server server;
    std::promise<void> eof;
    auto released = eof.get_future().share();
    server.add_tool({"wait"}, [&](const Json&, RequestContext&) {
      released.wait();
      return text_result("not cancelled");
    });
    auto invalid = make_notification("notifications/cancelled", {{"requestId", 1}});
    if (mutation == 0)
      invalid.erase("jsonrpc");
    else if (mutation == 1)
      invalid["jsonrpc"] = "1.0";
    else if (mutation == 2)
      invalid["result"] = Json::object();
    else if (mutation == 3)
      invalid["error"] = {{"code", 1}, {"message", "invalid notification"}};
    else
      invalid["params"]["reason"] = 42;
    EofSignalBuffer buffer(serialize_json(make_request(1, "tools/call", {{"name", "wait"}})) + "\n" + serialize_json(invalid) + "\n",
                           [&] { eof.set_value(); });
    std::istream input(&buffer);
    std::ostringstream output;
    run_stdio(server, input, output, {1, true});
    auto messages = stdio_messages(output.str());
    EXPECT_EQ(messages.size(), 1u);
    if (messages.size() != 1)
      continue;
    EXPECT_EQ(messages[0]["id"], 1);
    EXPECT_TRUE(messages[0].contains("result"));
  }
}

TEST(Stdio, CancelledSubscriptionEmitsNoFinalResponse) {
  Server server;
  std::promise<void> registered;
  auto registration = registered.get_future();
  auto subscription = make_request(1, "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}}}});
  // One worker executes the following discovery only after the subscription
  // is registered and has left the pending table. Cancel that active stream.
  GatedInputBuffer input_buffer(serialize_json(subscription) + "\n" + serialize_json(make_request(2, "server/discover")) + "\n",
                                serialize_json(make_notification("notifications/cancelled", {{"requestId", 1}})) + "\n",
                                [&] { EXPECT_EQ(registration.wait_for(std::chrono::seconds(5)), std::future_status::ready); });
  ResponseSignalBuffer output_buffer([&] { registered.set_value(); });
  std::istream input(&input_buffer);
  std::ostream output(&output_buffer);
  run_stdio(server, input, output, {1, true});
  const auto messages = stdio_messages(output_buffer.str());
  EXPECT_EQ(messages.size(), 2u);  // subscription acknowledgment and discovery response
  for (const auto& message : messages)
    if (message.contains("id"))
      EXPECT_NE(message["id"], 1) << serialize_json(message);
}

TEST(Stdio, CancelledQueuedSubscriptionEmitsNoMessages) {
  Server server;
  std::promise<void> eof;
  auto released = eof.get_future().share();
  server.add_tool({"wait"}, [&](const Json&, RequestContext&) {
    released.wait();
    return text_result("released");
  });
  auto subscription = make_request(2, "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}}}});
  EofSignalBuffer buffer(serialize_json(make_request(1, "tools/call", {{"name", "wait"}})) + "\n" + serialize_json(subscription) + "\n" +
                             serialize_json(make_notification("notifications/cancelled", {{"requestId", 2}})) + "\n",
                         [&] { eof.set_value(); });
  std::istream input(&buffer);
  std::ostringstream output;
  run_stdio(server, input, output, {1, true});
  auto messages = stdio_messages(output.str());
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0]["id"], 1);
  EXPECT_TRUE(messages[0].contains("result"));
}

TEST(Stdio, CancelMarkerPrecedesCooperativeStop) {
  detail::StdioRequestState state;
  ASSERT_TRUE(state.can_activate());
  state.mark_cancelled();
  // Model cancellation after releasing the table lock, while cooperative
  // request_stop (and its synchronous application callbacks) is delayed.
  EXPECT_FALSE(state.stop_token().stop_requested());
  EXPECT_TRUE(state.is_cancelled());
  EXPECT_FALSE(state.can_activate());
  state.request_stop();
  EXPECT_TRUE(state.stop_token().stop_requested());
}

TEST(Stdio, CancelledSubscriptionDuringRegistrationReleasesServerCapacity) {
  ServerOptions options;
  options.max_subscriptions = 1;
  Server server(options);
  server.add_extension("io.modelcontextprotocol/tasks", Json::object(), "tasks/get", [](const Json&, RequestContext&) { return Json::object(); });
  std::promise<void> entered, eof;
  auto registration = entered.get_future();
  auto release = eof.get_future().share();
  server.set_task_subscription_authorizer([&](const Json&, std::string_view) {
    entered.set_value();
    release.wait();
    return std::string("fixed-test-owner");
  });
  auto first = make_request(1, "subscriptions/listen", {{"notifications", {{"taskIds", {"one"}}}}},
                            {{"extensions", {{"io.modelcontextprotocol/tasks", Json::object()}}}});
  auto next = make_request(2, "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}}}});
  GatedInputBuffer input_buffer(
      serialize_json(first) + "\n",
      serialize_json(make_notification("notifications/cancelled", {{"requestId", 1}})) + "\n" + serialize_json(next) + "\n",
      [&] { EXPECT_EQ(registration.wait_for(std::chrono::seconds(5)), std::future_status::ready); }, [&] { eof.set_value(); });
  std::istream input(&input_buffer);
  std::ostringstream output;
  run_stdio(server, input, output, {1, true});
  const auto messages = stdio_messages(output.str());
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0]["method"], "notifications/subscriptions/acknowledged");
  EXPECT_EQ(messages[0]["params"]["_meta"][subscription_key], 2);
  EXPECT_EQ(messages[1]["id"], 2);
  ASSERT_TRUE(messages[1].contains("result"));
  EXPECT_EQ(messages[1]["result"]["resultType"], "complete");
}

TEST(Http, NonSuccessStatusNeverDeliversJsonOrSseNotificationsOrSuccess) {
  const auto request = make_request(71, "tools/call", {{"name", "work"}, {"_meta", {{"io.modelcontextprotocol/logLevel", "info"}}}});
  const auto notice = serialize_json(make_notification("notifications/message", {{"level", "info"}, {"data", "untrusted status"}}));
  const auto success = serialize_json({{"jsonrpc", "2.0"}, {"id", 71}, {"result", {{"resultType", "complete"}}}});
  const auto error = serialize_json(make_error(71, -32601, "Unknown method"));
  for (const unsigned status : {302U, 400U, 404U, 500U}) {
    for (const std::string body : {success, notice}) {
      SCOPED_TRACE(std::to_string(status) + " JSON");
      HttpPeer peer(status, "application/json", body, true);
      HttpTransport transport(HttpClientOptions{peer.url()});
      int callbacks = 0;
      EXPECT_THROW(transport.exchange(request, {}, [&](const Json&) { ++callbacks; }), TransportError);
      EXPECT_EQ(callbacks, 0);
    }
    for (const bool final_error : {false, true}) {
      SCOPED_TRACE(std::to_string(status) + " SSE");
      HttpPeer peer(status, "text/event-stream", "data: " + notice + "\n\ndata: " + (final_error ? error : success) + "\n\n");
      HttpTransport transport(HttpClientOptions{peer.url()});
      int callbacks = 0;
      if (final_error)
        EXPECT_EQ(transport.exchange(request, {}, [&](const Json&) { ++callbacks; })["error"]["code"], -32601);
      else
        EXPECT_THROW(transport.exchange(request, {}, [&](const Json&) { ++callbacks; }), TransportError);
      EXPECT_EQ(callbacks, 0);
    }
  }
}

TEST(Http, ErrorStatusPreservesOnlyMatchingJsonRpcErrors) {
  const auto request = make_request(71, "server/discover");
  for (const unsigned status : {400U, 404U}) {
    HttpPeer peer(status, "application/json", serialize_json(make_error(71, -32601, "Unknown")));
    HttpTransport transport(HttpClientOptions{peer.url()});
    EXPECT_EQ(transport.exchange(request, {})["error"]["code"], -32601);
  }
  HttpPeer peer(404, "application/json", serialize_json(make_error(72, -32601, "Other request")));
  HttpTransport transport(HttpClientOptions{peer.url()});
  EXPECT_THROW(transport.exchange(request, {}), TransportError);
}

TEST(Http, JsonBodiesContainOnlyMatchingResponsesAndNeverDeliverNotifications) {
  const auto request = make_request(71, "tools/call", {{"name", "work"}, {"_meta", {{"io.modelcontextprotocol/logLevel", "info"}}}});
  const auto notice = make_notification("notifications/message", {{"level", "info"}, {"data", "log"}});
  HttpPeer peer(200, "application/json", serialize_json(notice));
  HttpTransport transport(HttpClientOptions{peer.url()});
  int callbacks = 0;
  try {
    (void)transport.exchange(request, {}, [&](const Json&) { ++callbacks; });
    FAIL() << "A JSON HTTP body must be a response to the request";
  } catch (const TransportError& error) {
    EXPECT_EQ(error.kind(), TransportErrorKind::invalid_response);
  }
  EXPECT_EQ(callbacks, 0);
  const Json response = {{"jsonrpc", "2.0"}, {"id", 71}, {"result", {{"resultType", "complete"}}}};
  HttpPeer success(200, "application/json", serialize_json(response));
  HttpTransport successful(HttpClientOptions{success.url()});
  EXPECT_EQ(successful.exchange(request, {}), response);
  HttpPeer wrong(200, "application/json", serialize_json(make_error(72, -32601, "Other request")));
  HttpTransport mismatched(HttpClientOptions{wrong.url()});
  EXPECT_THROW(mismatched.exchange(request, {}), TransportError);
}

TEST(Http, MaximumNativeTimeoutDoesNotOverflow) {
  Harness harness;
  HttpTransport transport(HttpClientOptions{harness.url()});
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(LONG_MAX);
  const auto response = transport.exchange(make_request(71, "server/discover"), options);
  EXPECT_EQ(response["id"], 71);
  EXPECT_TRUE(response.contains("result"));
}

TEST(Http, UnknownMethodsUse404BeforeAnyStreamNotification) {
  Harness harness;
  for (const Json meta : {Json::object(), Json{{"progressToken", "p"}}, Json{{"io.modelcontextprotocol/logLevel", "info"}}}) {
    auto request = make_request(71, "unknown/method", {{"_meta", meta}});
    const auto result = post(harness.url(), serialize_json(request), headers(request));
    EXPECT_EQ(result.status, 404);
    const auto response = parse_json(result.body);
    EXPECT_EQ(response["id"], 71);
    EXPECT_EQ(response["error"]["code"], -32601);
  }
}

TEST(Stdio, LoggedRequestsAreSerializedWhileOtherTrafficRemainsConcurrent) {
  Client client(std::make_shared<StdioTransport>(logging_peer()));
  RequestOptions options;
  options.meta = {{"io.modelcontextprotocol/logLevel", "info"}, {"progressToken", "first"}};
  std::promise<void> entered;
  std::vector<Json> first_logs, second_logs;
  auto first = std::async(std::launch::async, [&] {
    return client.call_tool("logged", {{"label", "first"}, {"gate", true}}, options, [&](const Json& message) {
      if (message["method"] == "notifications/message")
        first_logs.push_back(message);
      else if (message["params"]["progress"] == 1)
        entered.set_value();
    });
  });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  auto second = std::async(std::launch::async, [&] {
    RequestOptions other;
    other.meta["io.modelcontextprotocol/logLevel"] = "warning";
    return client.call_tool("logged", {{"label", "second"}}, other, [&](const Json& message) { second_logs.push_back(message); });
  });
  EXPECT_EQ(second.wait_for(std::chrono::milliseconds(80)), std::future_status::timeout);
  EXPECT_NO_THROW(client.discover());
  RequestOptions progress;
  progress.meta["progressToken"] = "unlogged";
  int progress_count = 0;
  EXPECT_NO_THROW(client.call_tool("progress", Json::object(), progress, [&](const Json&) { ++progress_count; }));
  EXPECT_EQ(progress_count, 1);
  std::stop_source stop;
  std::promise<void> ack;
  auto subscribed = std::async(std::launch::async, [&] {
    RequestOptions listen;
    listen.stop = stop.get_token();
    EXPECT_THROW(client.listen({{"toolsListChanged", true}}, listen, [&](const Json&) { ack.set_value(); }), TransportError);
  });
  EXPECT_EQ(ack.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  stop.request_stop();
  subscribed.get();
  EXPECT_NO_THROW(client.call_tool("release"));
  EXPECT_EQ(first.get()["content"][0]["text"], "first");
  EXPECT_EQ(second.get()["content"][0]["text"], "second");
  ASSERT_EQ(first_logs.size(), 2U);
  EXPECT_EQ(first_logs[0]["params"]["data"], "first-begin");
  EXPECT_EQ(first_logs[0]["params"]["level"], "info");
  EXPECT_EQ(first_logs[1]["params"]["data"], "first-end");
  ASSERT_EQ(second_logs.size(), 1U);
  EXPECT_EQ(second_logs[0]["params"]["data"], "second-end");
  EXPECT_EQ(second_logs[0]["params"]["level"], "warning");
}

TEST(Stdio, MaximumTimeoutDoesNotOverflowOrdinaryOrLoggedRequests) {
  StdioTransport transport(logging_peer());
  RequestOptions options;
  options.timeout = std::chrono::milliseconds::max();
  for (const bool logging : {false, true}) {
    auto request = make_request(logging ? 74 : 73, "server/discover");
    if (logging)
      request["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "info";
    EXPECT_NO_THROW({
      const auto response = transport.exchange(request, options);
      EXPECT_TRUE(response.contains("result"));
      EXPECT_EQ(response["id"], request["id"]);
    });
  }
}

TEST(Stdio, MaximumShutdownTimeoutAllowsGracefulProcessExit) {
  auto options = logging_peer();
  options.arguments = {"graceful-close"};
  options.shutdown_timeout = std::chrono::milliseconds::max();
  StdioTransport transport(options);
  ASSERT_TRUE(transport.exchange(make_request(73, "server/discover"), {}).contains("result"));
  const auto start = std::chrono::steady_clock::now();
  transport.close();
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
}

TEST(Stdio, WaitingForLogSlotUsesOriginalDeadlineAndCancellationWithoutClosingTransport) {
  Client client(std::make_shared<StdioTransport>(logging_peer()));
  RequestOptions options;
  options.meta = {{"io.modelcontextprotocol/logLevel", "info"}, {"progressToken", "first"}};
  std::promise<void> entered;
  auto first = std::async(std::launch::async, [&] {
    return client.call_tool("logged", {{"gate", true}}, options, [&](const Json& message) {
      if (message["method"] == "notifications/progress" && message["params"]["progress"] == 1)
        entered.set_value();
    });
  });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  RequestOptions waiting;
  waiting.meta["io.modelcontextprotocol/logLevel"] = "info";
  waiting.timeout = std::chrono::milliseconds(80);
  int callbacks = 0;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THROW(client.call_tool("logged", Json::object(), waiting, [&](const Json&) { ++callbacks; }), TransportError);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
  std::stop_source source;
  waiting.timeout = std::chrono::seconds(3);
  waiting.stop = source.get_token();
  auto cancelled = std::async(std::launch::async, [&] {
    EXPECT_THROW(client.call_tool("logged", Json::object(), waiting, [&](const Json&) { ++callbacks; }), TransportError);
  });
  EXPECT_EQ(cancelled.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
  source.request_stop();
  EXPECT_EQ(cancelled.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  cancelled.get();
  EXPECT_EQ(callbacks, 0);
  EXPECT_NO_THROW(client.call_tool("release"));
  EXPECT_NO_THROW(first.get());
  EXPECT_NO_THROW(client.discover());
}

TEST(Stdio, AbandoningSentLoggedRequestClosesTransportBeforeLateLogsCanEscape) {
  for (const bool cancellation : {false, true}) {
    Client client(std::make_shared<StdioTransport>(logging_peer()));
    client.list_tools();
    RequestOptions options;
    options.meta = {{"io.modelcontextprotocol/logLevel", "info"}, {"progressToken", "active"}};
    options.timeout = std::chrono::milliseconds(100);
    std::stop_source stop;
    if (cancellation)
      options.stop = stop.get_token();
    EXPECT_THROW(client.call_tool("logged", {{"label", "old"}, {"late", true}}, options,
                                  [&](const Json& message) {
                                    if (cancellation && message["method"] == "notifications/progress")
                                      stop.request_stop();
                                  }),
                 TransportError);
    int callbacks = 0;
    RequestOptions next;
    next.meta["io.modelcontextprotocol/logLevel"] = "info";
    EXPECT_THROW(client.call_tool("logged", {{"label", "new"}}, next, [&](const Json&) { ++callbacks; }), TransportError);
    EXPECT_EQ(callbacks, 0);
    EXPECT_THROW(client.discover(), TransportError);
  }
}

TEST(Stdio, LogSlotWaitDoesNotRestartDeadlineAfterAcquisition) {
  Client client(std::make_shared<StdioTransport>(logging_peer()));
  RequestOptions first_options;
  first_options.meta = {{"io.modelcontextprotocol/logLevel", "info"}, {"progressToken", "first"}};
  // Hold the log slot locally so releasing it requires no extra peer round trip.
  std::promise<void> entered, release_first;
  auto released = release_first.get_future();
  auto first = std::async(std::launch::async, [&] {
    return client.call_tool("logged", Json::object(), first_options, [&](const Json& message) {
      if (message["method"] == "notifications/progress" && message["params"]["progress"] == 1) {
        entered.set_value();
        EXPECT_EQ(released.wait_for(std::chrono::seconds(3)), std::future_status::ready);
      }
    });
  });
  struct Release {
    std::promise<void>& promise;
    bool completed = false;
    void now() {
      if (!std::exchange(completed, true))
        promise.set_value();
    }
    ~Release() { now(); }
  } cleanup{release_first};
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  std::promise<void> second_started;
  std::atomic<bool> second_received{false};
  auto second = std::async(std::launch::async, [&] {
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(400);
    options.meta["io.modelcontextprotocol/logLevel"] = "info";
    options.meta["progressToken"] = "second";
    second_started.set_value();
    try {
      client.call_tool("logged", {{"late", true}, {"delayMs", 300}}, options, [&](const Json& message) {
        if (message["method"] == "notifications/progress" && message["params"]["progress"] == 1)
          second_received.store(true);
      });
      ADD_FAILURE() << "The original deadline must include waiting for the log slot";
    } catch (const TransportError& error) {
      EXPECT_EQ(error.kind(), TransportErrorKind::timeout);
    }
  });
  second_started.get_future().wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  cleanup.now();
  EXPECT_NO_THROW(first.get());
  second.get();
  // A timeout before sending correctly leaves the transport open; this test
  // must exercise a request that has already acquired the slot and been sent.
  EXPECT_TRUE(second_received.load()) << "The second request must acquire the slot and be sent";
  EXPECT_THROW(client.discover(), TransportError);
}

TEST(Stdio, LoggingCallbackCanMakeOrdinaryNestedCallAndExceptionsCloseTransport) {
  Client client(std::make_shared<StdioTransport>(logging_peer()));
  RequestOptions options;
  options.meta["io.modelcontextprotocol/logLevel"] = "info";
  int callbacks = 0;
  EXPECT_THROW(client.call_tool("logged", {{"late", true}}, options,
                                [&](const Json&) {
                                  ++callbacks;
                                  EXPECT_NO_THROW(client.discover());
                                  throw std::runtime_error("consumer stopped logging");
                                }),
               std::runtime_error);
  EXPECT_EQ(callbacks, 1);
  EXPECT_THROW(client.discover(), TransportError);
}
TEST(Stdio, LoggingCallbackDeadlineStopsFurtherDeliveryAndClosesTransport) {
  ProcessOptions process;
  process.executable = MCP_TEST_PEER;
  process.arguments = {"logging"};
  process.shutdown_timeout = std::chrono::milliseconds(20);
  StdioTransport transport(process);
  // This case measures callback time, after the subprocess and its shared
  // libraries have initialized. Startup deadlines are tested separately.
  ASSERT_TRUE(transport.exchange(make_request(9190, "server/discover"), {}).contains("result"));
  auto request = make_request(9191, "tools/call",
                              {{"name", "logged"}, {"arguments", Json::object()}, {"_meta", {{"io.modelcontextprotocol/logLevel", "info"}}}});
  RequestOptions options;
  options.timeout = std::chrono::seconds(2);
  int callbacks = 0;
  try {
    (void)transport.exchange(request, options, [&](const Json&) {
      if (++callbacks == 1)
        std::this_thread::sleep_for(options.timeout + std::chrono::milliseconds(50));
    });
    FAIL() << "Callback processing exceeded the original request deadline";
  } catch (const TransportError& error) {
    EXPECT_EQ(error.kind(), TransportErrorKind::timeout);
  }
  EXPECT_EQ(callbacks, 1);
  EXPECT_THROW(transport.exchange(make_request(9192, "server/discover"), {}), TransportError);
}

TEST(Stdio, ClosingTransportWakesActiveAndWaitingLoggedCalls) {
  Client client(std::make_shared<StdioTransport>(logging_peer()));
  RequestOptions options;
  options.meta = {{"io.modelcontextprotocol/logLevel", "info"}, {"progressToken", "first"}};
  std::promise<void> entered;
  auto first = std::async(std::launch::async, [&] {
    EXPECT_THROW(client.call_tool("logged", {{"gate", true}}, options,
                                  [&](const Json& message) {
                                    if (message["method"] == "notifications/progress" && message["params"]["progress"] == 1)
                                      entered.set_value();
                                  }),
                 TransportError);
  });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  auto waiting = std::async(std::launch::async, [&] { EXPECT_THROW(client.call_tool("logged", Json::object(), options), TransportError); });
  EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
  client.close();
  EXPECT_EQ(waiting.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_EQ(first.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  waiting.get();
  first.get();
}

TEST(Http, CancellationBeforeFirstNotificationReachesHandler) {
  for (const bool tls : {false, true}) {
    SCOPED_TRACE(tls ? "HTTPS" : "HTTP");
    HttpServerOptions options;
    if (tls) {
      options.certificate_file = std::string(MCP_FIXTURES) + "/localhost.crt";
      options.private_key_file = std::string(MCP_FIXTURES) + "/localhost.key";
    }
    Harness harness(options);
    std::promise<void> entered, cancelled;
    harness.server.add_tool({"before_notify"}, [&](const Json&, RequestContext& context) {
      entered.set_value();
      while (!context.stop_token().stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      cancelled.set_value();
      return text_result("cancelled");
    });
    HttpClientOptions http;
    http.url = harness.url(tls ? "https" : "http");
    http.ca_file = options.certificate_file;
    Client client(std::make_shared<HttpTransport>(http));
    std::stop_source source;
    RequestOptions request;
    request.stop = source.get_token();
    request.meta["io.modelcontextprotocol/logLevel"] = "info";
    auto call = std::async(std::launch::async, [&] { EXPECT_THROW(client.call_tool("before_notify", Json::object(), request), TransportError); });
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    source.request_stop();
    EXPECT_EQ(cancelled.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    harness.listener.stop();
    call.get();
  }
}

TEST(Http, TaskSubscriptionsUseAuthenticatedIdentityAndRequireCapability) {
  using namespace mcp::extensions;
  HttpServerOptions options;
  options.authenticate = [](const Headers& headers) -> std::optional<Json> {
    const auto it = headers.find("authorization");
    if (it != headers.end() && it->second == "Bearer alice")
      return Json{{"iss", "issuer"}, {"sub", "alice"}};
    if (it != headers.end() && it->second == "Bearer bob")
      return Json{{"iss", "issuer"}, {"sub", "bob"}};
    return Json::object();
  };
  Harness harness(options);
  auto store = std::make_shared<MemoryTaskStore>();
  const auto task =
      store->create(make_task("issuer\nalice", "private-task", TaskStatus::working, std::chrono::minutes(1), std::chrono::milliseconds(1)));
  install_tasks(harness.server, store);
  const auto request = make_request(83, "subscriptions/listen", {{"notifications", {{"taskIds", {"private-task"}}}}}, tasks_capability());
  for (const std::string credential : {"Bearer bob", ""}) {
    HttpClientOptions http;
    http.url = harness.url();
    if (!credential.empty())
      http.headers["Authorization"] = credential;
    HttpTransport transport(http);
    int callbacks = 0;
    auto response = transport.exchange(request, {}, [&](const Json&) { ++callbacks; });
    EXPECT_TRUE(response.contains("error"));
    auto forged = request;
    forged["params"]["_meta"]["iss"] = "issuer";
    forged["params"]["_meta"]["sub"] = "alice";
    response = transport.exchange(forged, {}, [&](const Json&) { ++callbacks; });
    EXPECT_TRUE(response.contains("error"));
    EXPECT_EQ(callbacks, 0);
  }
  HttpClientOptions http;
  http.url = harness.url();
  http.headers["Authorization"] = "Bearer alice";
  HttpTransport transport(http);
  const auto no_capability = make_request(84, "subscriptions/listen", {{"notifications", {{"taskIds", {"private-task"}}}}});
  int rejected_callbacks = 0;
  EXPECT_TRUE(transport.exchange(no_capability, {}, [&](const Json&) { ++rejected_callbacks; }).contains("error"));
  EXPECT_EQ(rejected_callbacks, 0);
  std::promise<void> ack, changed;
  std::stop_source stop;
  RequestOptions listen;
  listen.stop = stop.get_token();
  listen.timeout = std::chrono::seconds(3);
  std::vector<Json> delivered;
  auto result = std::async(std::launch::async, [&] {
    EXPECT_THROW(transport.exchange(request, listen,
                                    [&](const Json& message) {
                                      delivered.push_back(message);
                                      if (message["method"] == "notifications/subscriptions/acknowledged")
                                        ack.set_value();
                                      else
                                        changed.set_value();
                                    }),
                 TransportError);
  });
  EXPECT_EQ(ack.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  harness.server.notify("notifications/tasks", {{"taskId", "private-task"}, {"status", "working"}}, task.owner);
  EXPECT_EQ(changed.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  stop.request_stop();
  result.get();
  ASSERT_EQ(delivered.size(), 2U);
  EXPECT_EQ(delivered[1]["params"]["taskId"], "private-task");
  EXPECT_EQ(serialize_json(delivered).find("alice"), std::string::npos);
}

TEST(Stdio, TaskSubscriptionsRejectClientSuppliedOwnerMetadata) {
  auto options = logging_peer();
  options.arguments = {"tasks"};
  StdioTransport transport(options);
  auto request = make_request(83, "subscriptions/listen", {{"notifications", {{"taskIds", {"private-task"}}}}}, extensions::tasks_capability());
  request["params"]["_meta"]["iss"] = "issuer";
  request["params"]["_meta"]["sub"] = "alice";
  int callbacks = 0;
  EXPECT_TRUE(transport.exchange(request, {}, [&](const Json&) { ++callbacks; }).contains("error"));
  EXPECT_EQ(callbacks, 0);
}

TEST(Stdio, LoggedNotificationsRetainValidationAndQueueBounds) {
  for (const std::string mode : {"invalid-log", "log-flood"}) {
    SCOPED_TRACE(mode);
    auto options = logging_peer();
    options.arguments = {mode};
    options.limits.stream_queue_bytes = 2048;
    StdioTransport transport(options);
    const auto request = make_request(71, "tools/call", {{"name", "work"}, {"_meta", {{"io.modelcontextprotocol/logLevel", "info"}}}});
    try {
      (void)transport.exchange(request, {}, [](const Json&) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
      FAIL() << "Invalid or excessive logging must fail the request";
    } catch (const TransportError& error) {
      EXPECT_EQ(error.kind(), mode == "invalid-log" ? TransportErrorKind::invalid_response : TransportErrorKind::limit);
    }
    EXPECT_THROW(transport.exchange(make_request(72, "server/discover"), {}), TransportError);
  }
}
TEST(Http, RoundTripAndConnectionReuse) {
  for (const bool tls : {false, true}) {
    SCOPED_TRACE(tls ? "HTTPS" : "HTTP");
    HttpServerOptions options;
    if (tls) {
      options.certificate_file = std::string(MCP_FIXTURES) + "/localhost.crt";
      options.private_key_file = std::string(MCP_FIXTURES) + "/localhost.key";
    }
    Harness harness(options);
    HttpClientOptions http;
    http.url = harness.url(tls ? "https" : "http");
    http.ca_file = options.certificate_file;
    Client client(std::make_shared<HttpTransport>(http));
    EXPECT_EQ(client.discover()["supportedVersions"][0], "2026-07-28");
    EXPECT_EQ(client.list_tools().size(), 1U);
    for (int i = 0; i < 10; ++i)
      EXPECT_EQ(client.call_tool("echo", {{"message", "hello"}})["content"][0]["text"], "hello");
  }
}
TEST(Http, RawNormativeRequest) {
  Harness harness;
  auto r = make_request(1, "tools/list");
  const auto result = post(harness.url(), serialize_json(r), headers(r));
  EXPECT_EQ(result.status, 200);
  auto response = parse_json(result.body);
  EXPECT_EQ(response["result"]["resultType"], "complete");
  EXPECT_TRUE(response["result"].contains("ttlMs"));
}
TEST(Http, HeaderBodyMismatchesAreRejected) {
  Harness harness;
  auto r = make_request(1, "tools/call", {{"name", "echo"}, {"arguments", {{"message", "hello"}}}});
  for (const char* key : {"mcp-method", "mcp-name", "mcp-protocol-version"}) {
    auto h = headers(r);
    h[key] = "mismatch";
    const auto result = post(harness.url(), serialize_json(r), h);
    EXPECT_EQ(result.status, 400);
    EXPECT_EQ(parse_json(result.body)["error"]["code"], -32020);
    h = headers(r);
    h.erase(key);
    EXPECT_EQ(parse_json(post(harness.url(), serialize_json(r), h).body)["error"]["code"], -32020);
  }
}
TEST(Http, OriginAndAuthorizationAreCheckedBeforeDispatch) {
  HttpServerOptions options;
  options.allowed_origins = {"https://allowed.example"};
  options.authorize = [](const Headers& h) {
    const auto it = h.find("authorization");
    return it != h.end() && it->second == "Bearer good";
  };
  Harness harness(options);
  auto r = make_request(1, "tools/list");
  auto h = headers(r);
  h["origin"] = "https://evil.example";
  EXPECT_EQ(post(harness.url(), serialize_json(r), h).status, 403);
  h["origin"] = "https://allowed.example";
  EXPECT_EQ(post(harness.url(), serialize_json(r), h).status, 401);
  h["authorization"] = "Bearer good";
  EXPECT_EQ(post(harness.url(), serialize_json(r), h).status, 200);
}
TEST(Http, AuthenticatedAttributesCannotBeSpoofedByRequestMetadata) {
  HttpServerOptions options;
  options.authenticate = [](const Headers& headers) -> std::optional<Json> {
    const auto it = headers.find("authorization");
    if (it == headers.end() || it->second != "Bearer valid")
      return std::nullopt;
    return Json{{"subject", "verified"}};
  };
  Harness harness(options);
  harness.server.add_tool({"identity"}, [](const Json&, RequestContext& context) { return text_result(text(context.attributes()["subject"])); });
  HttpClientOptions http;
  http.url = harness.url();
  Client denied(std::make_shared<HttpTransport>(http));
  EXPECT_THROW(denied.discover(), std::exception);
  http.headers["Authorization"] = "Bearer valid";
  Client client(std::make_shared<HttpTransport>(http));
  RequestOptions request;
  request.meta["subject"] = "spoofed";
  EXPECT_EQ(client.call_tool("identity", Json::object(), request)["content"][0]["text"], "verified");
}
TEST(Stdio, IndependentClientsShareOneTransportWithoutIdCollisions) {
  auto transport = std::make_shared<StdioTransport>(ProcessOptions{MCP_STDIO_SERVER});
  Client first(transport), second(transport);
  auto a = std::async(std::launch::async, [&] {
    for (int i = 0; i < 30; ++i)
      EXPECT_NO_THROW(first.discover());
  });
  auto b = std::async(std::launch::async, [&] {
    for (int i = 0; i < 30; ++i)
      EXPECT_NO_THROW(second.discover());
  });
  a.get();
  b.get();
  first.close();
}
TEST(Http, MethodPathAndContentNegotiation) {
  Harness harness;
  auto r = make_request(1, "tools/list");
  auto h = headers(r);
  EXPECT_EQ(post(harness.url(), "", h, "GET").status, 405);
  EXPECT_EQ(post(harness.url(), "", h, "DELETE").status, 405);
  h["content-type"] = "text/plain";
  EXPECT_EQ(post(harness.url(), serialize_json(r), h).status, 415);
  h = headers(r);
  h["accept"] = "application/json";
  EXPECT_EQ(post(harness.url(), serialize_json(r), h).status, 406);
  EXPECT_EQ(post(harness.url() + "/wrong", serialize_json(r), headers(r)).status, 404);
}
TEST(Http, MalformedBodyAndUnknownVersion) {
  Harness harness;
  auto r = make_request(1, "tools/list");
  auto h = headers(r);
  EXPECT_EQ(post(harness.url(), "{", h).status, 400);
  r["params"]["_meta"][version_key] = "1900-01-01";
  h["mcp-protocol-version"] = "1900-01-01";
  auto result = post(harness.url(), serialize_json(r), h);
  EXPECT_EQ(result.status, 400);
  EXPECT_EQ(parse_json(result.body)["error"]["code"], -32022);
}
TEST(Http, NotificationReturnsEmpty202) {
  Harness harness;
  auto r = make_notification("example/notice");
  Headers h = {{"accept", "application/json, text/event-stream"}, {"content-type", "application/json"}};
  auto result = post(harness.url(), serialize_json(r), h);
  EXPECT_EQ(result.status, 202);
  EXPECT_TRUE(result.body.empty());
}
TEST(Http, SseProgressAndFinalResponse) {
  Harness harness;
  harness.server.add_tool({"work"}, [](const Json&, RequestContext& ctx) {
    ctx.progress(0, 2);
    ctx.progress(1, 2);
    return text_result("done");
  });
  Client client(std::make_shared<HttpTransport>(HttpClientOptions{harness.url()}));
  RequestOptions options;
  options.meta["progressToken"] = "progress";
  std::vector<Json> notifications;
  auto result = client.call_tool("work", Json::object(), options, [&](const Json& n) { notifications.push_back(n); });
  EXPECT_EQ(result["content"][0]["text"], "done");
  ASSERT_EQ(notifications.size(), 2U);
  EXPECT_EQ(notifications[1]["params"]["progress"], 1);
}
TEST(Http, ConcurrentCallsAreCorrelated) {
  Harness harness;
  Client client(std::make_shared<HttpTransport>(HttpClientOptions{harness.url()}));
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t)
    threads.emplace_back([&, t] {
      for (int i = 0; i < 10; ++i)
        EXPECT_EQ(client.call_tool("echo", {{"message", std::to_string(t)}})["content"][0]["text"], std::to_string(t));
    });
  for (auto& thread : threads)
    thread.join();
}
TEST(Http, SustainedKeepaliveAndShutdown) {
  Harness harness;
  Client client(std::make_shared<HttpTransport>(HttpClientOptions{harness.url()}));
  for (int i = 0; i < 1000; ++i)
    ASSERT_EQ(client.call_tool("echo", {{"message", "serial"}})["content"][0]["text"], "serial");
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&, t] {
      try {
        const auto expected = std::to_string(t);
        for (int i = 0; i < 250; ++i)
          if (client.call_tool("echo", {{"message", expected}})["content"][0]["text"] != expected)
            ++failures;
      } catch (...) {
        ++failures;
      }
    });
  for (auto& thread : threads)
    thread.join();
  EXPECT_EQ(failures, 0);
  client.close();
  harness.listener.stop();
}
TEST(Http, CancellationClosesStreamAndReachesServer) {
  for (const bool tls : {false, true}) {
    SCOPED_TRACE(tls ? "HTTPS" : "HTTP");
    HttpServerOptions listener;
    if (tls) {
      listener.certificate_file = std::string(MCP_FIXTURES) + "/localhost.crt";
      listener.private_key_file = std::string(MCP_FIXTURES) + "/localhost.key";
    }
    Harness harness(listener);
    std::promise<void> notified, cancelled;
    harness.server.add_tool({"wait"}, [&](const Json&, RequestContext& ctx) {
      ctx.progress(0, 1);
      while (!ctx.stop_token().stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      cancelled.set_value();
      ctx.throw_if_cancelled();
      return text_result("never");
    });
    HttpClientOptions http;
    http.url = harness.url(tls ? "https" : "http");
    http.ca_file = listener.certificate_file;
    Client client(std::make_shared<HttpTransport>(http));
    std::stop_source source;
    RequestOptions options;
    options.stop = source.get_token();
    options.meta["progressToken"] = "p";
    auto call = std::async(std::launch::async, [&] {
      EXPECT_THROW(client.call_tool("wait", Json::object(), options,
                                    [&](const Json& event) {
                                      if (event["method"] == "notifications/progress")
                                        notified.set_value();
                                    }),
                   TransportError);
    });
    EXPECT_EQ(notified.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    source.request_stop();
    call.get();
    EXPECT_EQ(cancelled.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_NO_THROW(client.discover());
  }
}
TEST(Http, SubscriptionsUseDedicatedStreamsWithoutOccupyingWorkers) {
  Harness harness;
  auto transport = std::make_shared<HttpTransport>(HttpClientOptions{harness.url()});
  Client client(transport);
  std::promise<void> ack, changed;
  std::stop_source source;
  RequestOptions options;
  options.stop = source.get_token();
  auto listen = std::async(std::launch::async, [&] {
    EXPECT_THROW(client.listen({{"toolsListChanged", true}}, options,
                               [&](const Json& n) {
                                 if (n["method"] == "notifications/subscriptions/acknowledged")
                                   ack.set_value();
                                 else if (n["method"] == "notifications/tools/list_changed")
                                   changed.set_value();
                               }),
                 TransportError);
  });
  ack.get_future().wait();
  EXPECT_NO_THROW(client.discover());
  harness.server.add_tool({"added"}, [](const Json&, RequestContext&) { return text_result("added"); });
  EXPECT_EQ(changed.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  source.request_stop();
  listen.get();
}
TEST(Http, CustomNestedHeaderAndUnicodeName) {
  Harness harness;
  const std::string name = "echo \xE4\xB8\xAD";
  harness.server.add_tool(
      {name, "",
       parse_json(
           R"({"type":"object","properties":{"options":{"type":"object","properties":{"region":{"type":"string","x-mcp-header":"Region"}}}}})")},
      [](const Json& args, RequestContext&) { return text_result(text(args["options"]["region"])); });
  Client client(std::make_shared<HttpTransport>(HttpClientOptions{harness.url()}));
  client.list_tools();
  EXPECT_EQ(client.call_tool(name, {{"options", {{"region", " padded \xE4\xB8\xAD "}}}})["content"][0]["text"], " padded \xE4\xB8\xAD ");
  auto r = make_request(1, "tools/call", {{"name", name}, {"arguments", {{"options", {{"region", "west"}}}}}});
  auto h = headers(r);
  EXPECT_EQ(parse_json(post(harness.url(), serialize_json(r), h).body)["error"]["code"], -32020);
}
TEST(Http, TlsVerifiesCertificateAndTrustedCaWorks) {
  HttpServerOptions options;
  options.certificate_file = std::string(MCP_FIXTURES) + "/localhost.crt";
  options.private_key_file = std::string(MCP_FIXTURES) + "/localhost.key";
  Harness harness(options);
  Client untrusted(std::make_shared<HttpTransport>(HttpClientOptions{harness.url("https")}));
  EXPECT_THROW(untrusted.discover(), TransportError);
  HttpClientOptions trusted;
  trusted.url = harness.url("https");
  trusted.ca_file = options.certificate_file;
  Client client(std::make_shared<HttpTransport>(trusted));
  EXPECT_NO_THROW(client.discover());
}
TEST(Http, TlsAbruptDisconnectBeforeNotificationReachesHandler) {
  std::promise<void> entered, cancelled;
  HttpServerOptions options;
  options.certificate_file = std::string(MCP_FIXTURES) + "/localhost.crt";
  options.private_key_file = std::string(MCP_FIXTURES) + "/localhost.key";
  Harness harness(options);
  harness.server.add_tool({"wait"}, [&](const Json&, RequestContext& context) {
    entered.set_value();
    while (!context.stop_token().stop_requested())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    cancelled.set_value();
    return text_result("cancelled");
  });
  TlsPeer peer(harness.listener.port());
  peer.send(make_request(1, "tools/call", {{"name", "wait"}, {"arguments", Json::object()}}));
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  peer.disconnect();
  EXPECT_EQ(cancelled.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
}
TEST(Http, PipelinedBytesDoNotSuppressDisconnectCancellation) {
  for (const bool tls : {false, true}) {
    SCOPED_TRACE(tls ? "HTTPS" : "HTTP");
    std::promise<void> entered, cancelled;
    HttpServerOptions options;
    if (tls) {
      options.certificate_file = std::string(MCP_FIXTURES) + "/localhost.crt";
      options.private_key_file = std::string(MCP_FIXTURES) + "/localhost.key";
    }
    Harness harness(options);
    harness.server.add_tool({"wait"}, [&](const Json&, RequestContext& context) {
      entered.set_value();
      while (!context.stop_token().stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      cancelled.set_value();
      return text_result("cancelled");
    });
    auto exercise = [&](auto& peer) {
      peer.send(make_request(1, "tools/call", {{"name", "wait"}, {"arguments", Json::object()}}));
      ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
      boost::asio::write(peer.stream, boost::asio::buffer("P", 1));
      // Give the pending monitor a chance to consume the pipelined byte.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      peer.disconnect();
      EXPECT_EQ(cancelled.get_future().wait_for(std::chrono::milliseconds(700)), std::future_status::ready);
    };
    if (tls) {
      TlsPeer peer(harness.listener.port());
      exercise(peer);
    } else {
      PlainPeer peer(harness.listener.port());
      exercise(peer);
    }
  }
}
TEST(Http, PendingPipelineBufferLimitCancelsHandler) {
  for (const bool tls : {false, true}) {
    SCOPED_TRACE(tls ? "HTTPS" : "HTTP");
    std::promise<void> entered, cancelled;
    ServerOptions server_options;
    server_options.limits.stream_queue_bytes = 16;
    Server server(server_options);
    server.add_tool({"wait"}, [&](const Json&, RequestContext& context) {
      entered.set_value();
      while (!context.stop_token().stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      cancelled.set_value();
      return text_result("cancelled");
    });
    HttpServerOptions options;
    if (tls) {
      options.certificate_file = std::string(MCP_FIXTURES) + "/localhost.crt";
      options.private_key_file = std::string(MCP_FIXTURES) + "/localhost.key";
    }
    HttpServer listener(server, options);
    listener.start();
    auto exercise = [&](auto& peer) {
      peer.send(make_request(1, "tools/call", {{"name", "wait"}, {"arguments", Json::object()}}));
      ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
      boost::asio::write(peer.stream, boost::asio::buffer("abcdefghijklmnop", 16));
      EXPECT_EQ(cancelled.get_future().wait_for(std::chrono::milliseconds(700)), std::future_status::ready);
    };
    if (tls) {
      TlsPeer peer(listener.port());
      exercise(peer);
    } else {
      PlainPeer peer(listener.port());
      exercise(peer);
    }
  }
}
TEST(Http, PipelinedRequestBytesSurviveDisconnectMonitoring) {
  for (const bool tls : {false, true}) {
    SCOPED_TRACE(tls ? "HTTPS" : "HTTP");
    std::promise<void> entered;
    std::atomic<bool> release{false};
    HttpServerOptions options;
    if (tls) {
      options.certificate_file = std::string(MCP_FIXTURES) + "/localhost.crt";
      options.private_key_file = std::string(MCP_FIXTURES) + "/localhost.key";
    }
    Harness harness(options);
    struct Release {
      std::atomic<bool>& value;
      ~Release() { value.store(true); }
    } cleanup{release};
    harness.server.add_tool({"first"}, [&](const Json&, RequestContext& context) {
      entered.set_value();
      while (!release.load() && !context.stop_token().stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return text_result("first");
    });
    auto exercise = [&](auto& peer) {
      peer.send(make_request(1, "tools/call", {{"name", "first"}, {"arguments", Json::object()}}));
      ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
      peer.send(make_request(2, "tools/call", {{"name", "echo"}, {"arguments", {{"message", "second"}}}}));
      release.store(true);
      auto response = peer.receive();
      EXPECT_EQ(response["id"], 1);
      EXPECT_EQ(response["result"]["content"][0]["text"], "first");
      response = peer.receive();
      EXPECT_EQ(response["id"], 2);
      EXPECT_EQ(response["result"]["content"][0]["text"], "second");
    };
    if (tls) {
      TlsPeer peer(harness.listener.port());
      exercise(peer);
    } else {
      PlainPeer peer(harness.listener.port());
      exercise(peer);
    }
  }
}
TEST(Http, CloseCancelsPendingAndRejectsNewCalls) {
  Harness harness;
  auto transport = std::make_shared<HttpTransport>(HttpClientOptions{harness.url()});
  Client client(transport);
  client.close();
  EXPECT_THROW(client.discover(), TransportError);
}
TEST(Http, ListenerStopIsIdempotent) {
  Harness harness;
  harness.listener.stop();
  harness.listener.stop();
  EXPECT_THROW(harness.listener.start(), std::logic_error);
}
TEST(Http, UnsafeHeadersAndNonHttpSchemesAreRejected) {
  EXPECT_THROW(HttpTransport(HttpClientOptions{"file:///etc/passwd"}), std::invalid_argument);
  HttpClientOptions options;
  options.url = "http://localhost/";
  options.headers["Authorization"] = "bad\r\nInjected: x";
  EXPECT_THROW((void)HttpTransport(options), std::invalid_argument);
}
TEST(Http, CloseAbortsAnActiveExchange) {
  Harness harness;
  std::promise<void> entered;
  harness.server.add_tool({"wait"}, [&](const Json&, RequestContext& context) {
    entered.set_value();
    while (!context.stop_token().stop_requested())
      std::this_thread::yield();
    return text_result("cancelled");
  });
  auto transport = std::make_shared<HttpTransport>(HttpClientOptions{harness.url()});
  Client client(transport);
  RequestOptions options;
  auto result = std::async(std::launch::async, [&] { EXPECT_THROW(client.call_tool("wait", Json::object(), options), TransportError); });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  client.close();
  EXPECT_EQ(result.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  result.get();
}
TEST(Http, HandlerCanSignalStopWithoutJoiningItself) {
  Harness harness;
  std::atomic<bool> stopped_from_handler{false};
  harness.server.add_tool({"stop"}, [&](const Json&, RequestContext&) {
    harness.listener.stop();
    stopped_from_handler.store(true);
    return text_result("stopping");
  });
  Client client(std::make_shared<HttpTransport>(HttpClientOptions{harness.url()}));
  try {
    (void)client.call_tool("stop");
  } catch (const std::exception&) {}
  harness.listener.stop();
  EXPECT_TRUE(stopped_from_handler.load());
}
TEST(Http, CancelSnapshotKeepsLastChannelAliveUntilRegistryMutexIsReleased) {
  struct Channel {
    std::function<void()> cancelled, destroyed;
    void cancel() { cancelled(); }
    ~Channel() { destroyed(); }
  };
  std::mutex channels_mutex;
  std::map<unsigned, std::weak_ptr<Channel>> channels;
  std::atomic<bool> destroyed{false};
  auto channel = std::make_shared<Channel>();
  channel->destroyed = [&] {
    // This is the production Session destructor's registry re-entry.
    std::lock_guard lock(channels_mutex);
    channels.erase(1);
    destroyed.store(true);
  };
  auto peer_owner = channel;
  channel->cancelled = [&] {
    // Force the other IO thread to release every owner except the snapshot.
    std::thread closer([&] { peer_owner.reset(); });
    closer.join();
  };
  channels.emplace(1, channel);
  channel.reset();
  auto snapshot = detail::snapshot_live_channels(channels_mutex, channels);
  ASSERT_EQ(snapshot.size(), 1U);
  snapshot.front()->cancel();
  EXPECT_FALSE(destroyed.load());
  snapshot.clear();
  EXPECT_TRUE(destroyed.load());
  std::lock_guard lock(channels_mutex);
  EXPECT_TRUE(channels.empty());
}
TEST(Http, IdleSubscriptionsSurviveOrdinaryConnectionTimeout) {
  HttpServerOptions opts;
  opts.idle_timeout = std::chrono::milliseconds(100);
  Harness harness(opts);
  Client client(std::make_shared<HttpTransport>(HttpClientOptions{harness.url()}));
  std::promise<void> ack, changed;
  std::stop_source source;
  RequestOptions options;
  options.stop = source.get_token();
  auto result = std::async(std::launch::async, [&] {
    EXPECT_THROW(client.listen({{"toolsListChanged", true}}, options,
                               [&](const Json& message) {
                                 if (message["method"] == "notifications/subscriptions/acknowledged")
                                   ack.set_value();
                                 else
                                   changed.set_value();
                               }),
                 TransportError);
  });
  ASSERT_EQ(ack.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  harness.server.add_tool({"after_idle"}, [](const Json&, RequestContext&) { return text_result("ok"); });
  EXPECT_EQ(changed.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  source.request_stop();
  result.get();
}
TEST(Notifications, RejectUpdatesBeforeAckAndUnrequestedFilters) {
  auto request = make_request(1, "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}}}});
  auto update = make_notification("notifications/tools/list_changed", {{"_meta", {{subscription_key, 1}}}});
  detail::NotificationState state;
  EXPECT_THROW(detail::validate_notification(update, request, state), TransportError);
  auto ack = make_notification("notifications/subscriptions/acknowledged",
                               {{"notifications", {{"promptsListChanged", true}}}, {"_meta", {{subscription_key, 1}}}});
  EXPECT_THROW(detail::validate_notification(ack, request, state), TransportError);
  ack["params"]["notifications"] = request["params"]["notifications"];
  EXPECT_NO_THROW(detail::validate_notification(ack, request, state));
  EXPECT_THROW(detail::validate_notification(ack, request, state), TransportError);
  EXPECT_NO_THROW(detail::validate_notification(update, request, state));
  update["params"]["_meta"][subscription_key] = 2;
  EXPECT_THROW(detail::validate_notification(update, request, state), TransportError);
}
TEST(Notifications, RejectNonIncreasingProgress) {
  auto request = make_request(1, "tools/call", {{"name", "x"}, {"_meta", {{"progressToken", "p"}}}});
  auto update = make_notification("notifications/progress", {{"progressToken", "p"}, {"progress", 2}});
  detail::NotificationState state;
  EXPECT_NO_THROW(detail::validate_notification(update, request, state));
  EXPECT_THROW(detail::validate_notification(update, request, state), TransportError);
  update["params"]["progress"] = 1;
  EXPECT_THROW(detail::validate_notification(update, request, state), TransportError);
}
TEST(Stdio, MalformedOversizedCrashedAndMismatchedPeersFailPromptly) {
  for (const char* mode : {"malformed", "oversize", "crash", "wrong-id"}) {
    ProcessOptions process;
    process.executable = MCP_TEST_PEER;
    process.arguments = {mode};
    process.limits.message_bytes = 1024;
    process.shutdown_timeout = std::chrono::milliseconds(20);
    Client client(std::make_shared<StdioTransport>(process));
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(100);
    EXPECT_THROW(client.discover(options), TransportError) << mode;
    client.close();
  }
}
TEST(Stdio, NotificationCallbacksCanMakeNestedRequestsAndClose) {
  ProcessOptions process;
  process.executable = MCP_TEST_PEER;
  Client client(std::make_shared<StdioTransport>(process));
  RequestOptions options;
  options.meta["progressToken"] = "nested";
  auto result = client.call_tool("progress", Json::object(), options, [&](const Json&) { EXPECT_NO_THROW(client.discover()); });
  EXPECT_EQ(result["content"][0]["text"], "done");
  EXPECT_THROW(client.call_tool("progress", Json::object(), options, [&](const Json&) { client.close(); }), TransportError);
}
class HeaderCodec : public testing::TestWithParam<std::string> {};
TEST_P(HeaderCodec, RoundTrip) {
  EXPECT_EQ(detail::decode_header(detail::encode_header(GetParam())), GetParam());
}
INSTANTIATE_TEST_SUITE_P(Values, HeaderCodec,
                         testing::Values("", "west", " padded ", "line\nbreak", "a\tb", "=?base64?literal?=", "\xE4\xB8\xAD\xE6\x96\x87"));
TEST(HeaderCodec, NonCanonicalBase64IsRejected) {
  for (const char* value : {"=?base64?a?=", "=?base64?!!!!?=", "=?base64?YR==?=", "=?base64?YWJ=?=", "=?base64?/w==?="})
    EXPECT_THROW(detail::decode_header(value), RpcError);
}
TEST(HeaderCodec, InvalidAnnotationsAreRejectedAtRegistration) {
  Server server;
  for (const Json& schema : {parse_json(R"({"type":"object","x-mcp-header":"x"})"),
                             parse_json(R"({"type":"object","properties":{"x":{"type":"object","x-mcp-header":"X"}}})"),
                             parse_json(R"({"type":"object","oneOf":[{"properties":{"x":{"type":"string","x-mcp-header":"X"}}}]})")})
    EXPECT_THROW(server.add_tool({"bad", "", schema}, [](const Json&, RequestContext&) { return text_result("bad"); }), std::invalid_argument);
}
TEST(Stdio, SubprocessRoundTripAndConcurrentRequests) {
  auto transport = std::make_shared<StdioTransport>(ProcessOptions{MCP_STDIO_SERVER});
  Client client(transport);
  EXPECT_NO_THROW(client.discover());
  EXPECT_EQ(client.list_tools().size(), 3U);
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i)
    threads.emplace_back([&, i] { EXPECT_EQ(client.call_tool("echo", {{"message", std::to_string(i)}})["content"][0]["text"], std::to_string(i)); });
  for (auto& thread : threads)
    thread.join();
  client.close();
  EXPECT_THROW(client.discover(), TransportError);
}
TEST(Stdio, TimeoutCancelsAndOtherRequestsStillWork) {
  auto transport = std::make_shared<StdioTransport>(ProcessOptions{MCP_STDIO_SERVER});
  Client client(transport);
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(50);
  EXPECT_THROW(client.call_tool("slow", Json::object(), options), TransportError);
  EXPECT_NO_THROW(client.discover());
  client.close();
}
TEST(Stdio, SubscriptionCancellationAndMultiplexing) {
  auto transport = std::make_shared<StdioTransport>(ProcessOptions{MCP_STDIO_SERVER});
  Client client(transport);
  std::promise<void> ack;
  std::stop_source source;
  RequestOptions options;
  options.stop = source.get_token();
  auto listening = std::async(std::launch::async, [&] {
    EXPECT_THROW(client.listen({{"toolsListChanged", true}}, options, [&](const Json&) { ack.set_value(); }), TransportError);
  });
  ack.get_future().wait();
  EXPECT_NO_THROW(client.discover());
  source.request_stop();
  listening.get();
  client.close();
}
TEST(Stdio, FiniteStreamsAndOversizedLines) {
  Server server;
  auto request = make_request(1, "server/discover");
  std::istringstream input(serialize_json(request) + "\n");
  std::ostringstream output;
  run_stdio(server, input, output, {4, true});
  EXPECT_EQ(parse_json(output.str())["id"], 1);
  ServerOptions options;
  options.limits.message_bytes = 512;
  Server bounded(options);
  std::istringstream oversized(std::string(600, 'x') + "\n" + serialize_json(request) + "\n");
  std::ostringstream results;
  run_stdio(bounded, oversized, results);
  EXPECT_NE(results.str().find("-32700"), std::string::npos);
}
namespace {
std::string process_path_text(const std::filesystem::path& path) {
  const auto bytes = path.u8string();
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
struct ProcessTempDirectory {
  std::filesystem::path root = std::filesystem::temp_directory_path();
  std::filesystem::path path = root / ("mcp-process-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ProcessTempDirectory() { std::filesystem::create_directories(path); }
  ~ProcessTempDirectory() {
    std::error_code error;
    if (path.parent_path() == root)
      std::filesystem::remove_all(path, error);
  }
};
struct SyntheticParentEnvironment {
  std::string name, previous;
  bool present = false;
  static void set(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
  }
  SyntheticParentEnvironment(std::string key, std::string value) : name(std::move(key)) {
    if (const auto old = std::getenv(name.c_str())) {
      present = true;
      previous = old;
    }
    set(name, value);
  }
  ~SyntheticParentEnvironment() {
    if (present)
      set(name, previous);
    else {
#ifdef _WIN32
      _putenv_s(name.c_str(), "");
#else
      unsetenv(name.c_str());
#endif
    }
  }
};
class ObservedProcess {
#ifdef _WIN32
  HANDLE handle_;
#else
  int pid_;
#endif
 public:
  explicit ObservedProcess(unsigned long id)
#ifdef _WIN32
      : handle_(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, id)){}
#else
      : pid_(static_cast<int>(id)) {
  }
#endif
        bool exited(std::chrono::milliseconds timeout) const {
#ifdef _WIN32
    return handle_ && WaitForSingleObject(handle_, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (kill(pid_, 0) == -1 && errno == ESRCH)
        return true;
#ifdef __linux__
      // An orphan can briefly remain a zombie until its new parent reaps it.
      std::ifstream status("/proc/" + std::to_string(pid_) + "/stat");
      std::string line;
      std::getline(status, line);
      const auto end = line.rfind(')');
      if (end != std::string::npos && end + 2 < line.size() && line[end + 2] == 'Z')
        return true;
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
#endif
  }
  ~ObservedProcess() {
#ifdef _WIN32
    if (handle_) {
      if (!exited(std::chrono::milliseconds(0)))
        TerminateProcess(handle_, 1);
      CloseHandle(handle_);
    }
#else
    if (!exited(std::chrono::milliseconds(0)))
      kill(pid_, SIGKILL);
#endif
  }
};
ProcessOptions process_info_options() {
  ProcessOptions options;
  options.executable = MCP_TEST_PEER;
  options.arguments = {"process-info"};
  options.shutdown_timeout = std::chrono::milliseconds(20);
  return options;
}
}  // namespace
TEST(Stdio, ProcessEnvironmentInheritanceAndExplicitEmptyEnvironment) {
  SyntheticParentEnvironment parent("MCP_PROCESS_PARENT_ONLY", "inherited");
  SyntheticParentEnvironment overridden("MCP_PROCESS_OVERRIDE", "parent");
  for (bool inherit : {true, false}) {
    auto options = process_info_options();
    options.inherit_environment = inherit;
    options.environment["MCP_PROCESS_OVERRIDE"] = "child";
    StdioTransport transport(options);
    const auto result = transport.exchange(make_request(1, "server/discover"), {})["result"];
    EXPECT_EQ(result["parent"], inherit ? "inherited" : "");
    EXPECT_EQ(result["override"], "child");
#ifdef _WIN32
    EXPECT_EQ(result["hidden"], true);
#endif
  }
  auto options = process_info_options();
  options.inherit_environment = false;
  StdioTransport transport(options);
  const auto result = transport.exchange(make_request(2, "server/discover"), {})["result"];
  EXPECT_EQ(result["parent"], "");
  EXPECT_EQ(result["override"], "");
}
TEST(Stdio, ProcessUnicodeExecutableDirectoryArgumentsAndEnvironment) {
  ProcessTempDirectory temporary;
  const auto directory = temporary.path / std::filesystem::u8path("运行 目录");
  std::filesystem::create_directory(directory);
  const auto source = std::filesystem::u8path(MCP_TEST_PEER);
  const auto executable = directory / std::filesystem::u8path("测试 peer" + source.extension().string());
  std::filesystem::copy_file(source, executable);
#ifdef _WIN32
  // Keep the copied fixture's DLL lookup independent of inherited PATH.
  for (const auto& file : std::filesystem::directory_iterator(source.parent_path()))
    if (file.path().extension() == ".dll")
      std::filesystem::copy_file(file.path(), directory / file.path().filename());
#endif
  auto options = process_info_options();
  options.executable = process_path_text(executable);
  options.working_directory = process_path_text(directory);
  options.arguments = {"process-info", "中文 参数", "", "quote\" and trailing\\"};
  options.environment["MCP_PROCESS_UNICODE"] = "环境 值";
  StdioTransport transport(options);
  const auto result = transport.exchange(make_request(1, "server/discover"), {})["result"];
  EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::u8path(std::string(text(result["cwd"]))), directory));
  EXPECT_EQ(result["arguments"], Json::array({"中文 参数", "", "quote\" and trailing\\"}));
  EXPECT_EQ(result["unicode"], "环境 值");
  transport.close();
}
TEST(Stdio, ProcessCloseTerminatesDescendantsEvenAfterPrimaryExit) {
  for (const char* mode : {"tree-live", "tree-exit"}) {
    SCOPED_TRACE(mode);
    auto options = process_info_options();
    options.arguments = {mode};
    StdioTransport transport(options);
    const auto result = transport.exchange(make_request(1, "server/discover"), {})["result"];
    ObservedProcess descendant(result["descendant"].get<unsigned long>());
    EXPECT_FALSE(descendant.exited(std::chrono::milliseconds(0)));
    if (std::string_view(mode) == "tree-exit") {
      ObservedProcess primary(result["primary"].get<unsigned long>());
      EXPECT_TRUE(primary.exited(std::chrono::seconds(2)));
    }
    const auto start = std::chrono::steady_clock::now();
    transport.close();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    EXPECT_TRUE(descendant.exited(std::chrono::seconds(2)));
  }
}
TEST(Stdio, ProcessRejectsEmbeddedNulInsteadOfTruncatingLaunchInputs) {
  for (int field = 0; field < 3; ++field) {
    auto options = process_info_options();
    if (field == 0)
      options.executable += std::string("\0ignored", 8);
    if (field == 1)
      options.working_directory = process_path_text(std::filesystem::current_path()) + std::string("\0ignored", 8);
    if (field == 2)
      options.arguments.push_back(std::string("value\0ignored", 13));
    EXPECT_THROW({ StdioTransport transport(options); }, std::invalid_argument);
  }
}
TEST(Stdio, ProcessStartupFailureDoesNotPreventSubsequentLaunch) {
  auto options = process_info_options();
  options.executable += ".missing";
  EXPECT_ANY_THROW({ StdioTransport transport(options); });
  StdioTransport transport(process_info_options());
  EXPECT_TRUE(transport.exchange(make_request(1, "server/discover"), {}).contains("result"));
}
#ifndef _WIN32
TEST(Stdio, ProcessEnvironmentOwnsBytesAfterHelperReturnAndHeapChurn) {
  namespace bp = boost::process::v2;
  const std::string expected(8192, 'x');
  auto environment = detail::process_environment(false, {{"MCP_PROCESS_OVERRIDE", expected}});
  // A borrowed envp array can otherwise point into the helper's destroyed
  // local vector. Reuse freed allocation sizes before an actual execve.
  std::vector<std::string> heap_churn(512, std::string(8192, '!'));
  boost::asio::io_context io;
  boost::asio::writable_pipe input(io);
  boost::asio::readable_pipe output(io);
  bp::process child(io, detail::process_path(MCP_TEST_PEER), std::vector<std::string>{"process-info"}, bp::process_stdio{.in = input, .out = output},
                    environment);
  const auto request = serialize_json(make_request(1, "server/discover")) + '\n';
  boost::asio::write(input, boost::asio::buffer(request));
  boost::asio::streambuf buffer;
  boost::asio::read_until(output, buffer, '\n');
  std::istream stream(&buffer);
  std::string line;
  std::getline(stream, line);
  EXPECT_EQ(parse_json(line)["result"]["override"], expected);
  boost::system::error_code error;
  input.close(error);
  output.close(error);
  EXPECT_EQ(child.wait(), 0);
}
#endif

namespace {
class ToolListingHttpPeer {
 public:
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor{io, {boost::asio::ip::address_v4::loopback(), 0}};
  std::atomic<bool> stopping{false};
  std::future<void> worker;
  explicit ToolListingHttpPeer(std::function<Json(const Json&, const Request&)> handle) {
    acceptor.non_blocking(true);
    worker = std::async(std::launch::async, [this, handle = std::move(handle)] {
      namespace http = boost::beast::http;
      while (!stopping.load()) {
        boost::asio::ip::tcp::socket socket(io);
        boost::system::error_code error;
        acceptor.accept(socket, error);
        if (error == boost::asio::error::would_block || error == boost::asio::error::try_again) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        if (error)
          throw boost::system::system_error(error);
        boost::beast::flat_buffer buffer;
        Request request;
        http::read(socket, buffer, request);
        auto message = parse_json(request.body());
        auto result = handle(message, request);
        http::response<http::string_body> response(http::status::ok, 11);
        response.set(http::field::content_type, "application/json");
        response.body() = serialize_json(Json{{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", result}});
        response.keep_alive(false);
        response.prepare_payload();
        http::write(socket, response, error);
      }
    });
  }
  ~ToolListingHttpPeer() {
    stopping.store(true);
    worker.wait();
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/mcp"; }
};
}  // namespace
TEST(Http, InvalidHeaderToolsAreFilteredWithoutPoisoningHealthyToolsAndRelistingClearsCaches) {
  const Json valid_schema = {{"type", "object"}, {"properties", {{"region", {{"type", "string"}, {"x-mcp-header", "Region"}}}}}};
  auto bad_space = valid_schema;
  bad_space["properties"]["region"]["x-mcp-header"] = "bad header";
  auto bad_duplicate = valid_schema;
  bad_duplicate["properties"]["other"] = {{"type", "string"}, {"x-mcp-header", "region"}};
  auto bad_root = valid_schema;
  bad_root["x-mcp-header"] = "Root";
  auto bad_type = valid_schema;
  bad_type["properties"]["region"]["type"] = "number";
  const Json bad_branch = {{"type", "object"}, {"allOf", Json::array({valid_schema})}};
  const std::vector<Json> invalid = {bad_space, bad_duplicate, bad_root, bad_type, bad_branch};
  std::atomic<bool> invalidate_rotating{false};
  std::atomic<int> echo_calls{0}, rotating_calls{0}, listings{0};
  ToolListingHttpPeer peer([&](const Json& request, const ToolListingHttpPeer::Request& wire) {
    if (request["method"] == "tools/list") {
      ++listings;
      Json tools = Json::array(
          {{{"name", "echo"}, {"inputSchema", {{"type", "object"}}}},
           {{"name", "rotating"}, {"inputSchema", invalidate_rotating.load() ? bad_space : valid_schema}, {"outputSchema", {{"type", "integer"}}}}});
      for (std::size_t i = 0; i < invalid.size(); ++i)
        tools.push_back({{"name", "invalid-" + std::to_string(i)}, {"inputSchema", invalid[i]}});
      return Json{{"tools", std::move(tools)}};
    }
    EXPECT_EQ(request["method"], "tools/call");
    const auto name = request["params"]["name"];
    if (name == "echo") {
      ++echo_calls;
      return text_result("healthy");
    }
    EXPECT_EQ(name, "rotating");
    ++rotating_calls;
    const auto header = wire.find("mcp-param-region");
    if (invalidate_rotating.load())
      EXPECT_EQ(header, wire.end());
    else {
      EXPECT_NE(header, wire.end());
      if (header != wire.end())
        EXPECT_EQ(header->value(), "eu");
    }
    auto result = text_result("rotating");
    result["structuredContent"] = invalidate_rotating.load() ? Json("new untyped result") : Json(1);
    return result;
  });
  HttpClientOptions http;
  http.url = peer.url();
  auto transport = std::make_shared<HttpTransport>(http);
  Client client(transport);
  Json tools;
  ASSERT_NO_THROW(tools = client.list_tools());
  ASSERT_EQ(tools.size(), 2u);
  EXPECT_EQ(tools[0]["name"], "echo");
  EXPECT_EQ(tools[1]["name"], "rotating");
  EXPECT_EQ(client.call_tool("rotating", {{"region", "eu"}})["structuredContent"], 1);
  invalidate_rotating.store(true);
  ASSERT_NO_THROW(tools = client.list_tools());
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0]["name"], "echo");
  EXPECT_EQ(client.call_tool("echo")["content"][0]["text"], "healthy");
  // An explicit call by name must not reuse the now-filtered descriptor's
  // old output schema or its old mirrored HTTP argument headers.
  EXPECT_EQ(client.call_tool("rotating", {{"region", "eu"}})["structuredContent"], "new untyped result");
  EXPECT_EQ(echo_calls.load(), 1);
  EXPECT_EQ(rotating_calls.load(), 2);
  EXPECT_EQ(listings.load(), 3);
}

TEST(Stdio, NativeThreadStartFailureReleasesLaunchedProcess) {
#if defined(_MSC_VER) && defined(_DEBUG)
  auto direct_children = [] {
    std::set<DWORD> children;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
      throw std::system_error(GetLastError(), std::system_category());
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
      do {
        if (entry.th32ParentProcessID == GetCurrentProcessId())
          children.insert(entry.th32ProcessID);
      } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return children;
  };
  const auto before = direct_children();
  auto storage = std::make_shared<std::pmr::synchronized_pool_resource>();
  const std::weak_ptr<std::pmr::memory_resource> retained = storage;
  auto options = process_info_options();
  options.arguments = {"tree-leaf"};
  options.memory_resource = std::move(storage);
  // Only a thread-start allocation on this calling thread is rejected; the
  // child and its native process-tree owner have already been constructed.
  mcp_test::AllocationFault fault("*thread::_Start*StdioTransport*", sizeof(std::shared_ptr<void>), 1, GetCurrentThreadId());
  ASSERT_TRUE(fault.available()) << "StdioTransport thread-start symbols must be available";
  fault.arm();
  EXPECT_THROW({ StdioTransport transport(std::move(options)); }, std::bad_alloc);
  fault.disarm();
  ASSERT_TRUE(fault.rejected()) << "The actual native thread-capture allocation must fail";
  options.memory_resource.reset();
  EXPECT_TRUE(retained.expired()) << "An unstarted io_context must not retain its own Impl through a read callback";
  const auto after = direct_children();
  for (const auto pid : after) {
    if (before.contains(pid))
      continue;
    // Match only a newly created direct fixture child, then keep its exact
    // handle. Never enumerate/terminate other tests' existing processes.
    const auto handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) {
      EXPECT_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
      continue;
    }
    wchar_t image[32768];
    DWORD size = 32768;
    const bool fixture = QueryFullProcessImageNameW(handle, 0, image, &size) &&
                         std::filesystem::path(std::wstring(image, size)).filename() == std::filesystem::u8path(MCP_TEST_PEER).filename();
    if (fixture) {
      const auto status = WaitForSingleObject(handle, 2000);
      EXPECT_EQ(status, WAIT_OBJECT_0) << "Thread construction failure left its launched child alive";
      if (status != WAIT_OBJECT_0) {
        TerminateProcess(handle, 1);
        WaitForSingleObject(handle, 2000);
      }
    }
    CloseHandle(handle);
  }
#else
  GTEST_SKIP() << "Native CRT thread allocation injection requires the MSVC Debug runtime";
#endif
}

TEST(Stdio, NativeCloseAllocationFailureDoesNotTerminate) {
#if defined(_MSC_VER) && defined(_DEBUG)
  // Keep a noexcept failure isolated. Windows closes the child process's job
  // handle even on terminate(), so the synthetic fixture cannot escape.
  EXPECT_EXIT(
      {
        auto options = process_info_options();
        options.arguments = {"tree-leaf"};
        options.shutdown_timeout = std::chrono::milliseconds(0);
        StdioTransport transport(options);
        mcp_test::AllocationFault fault("*StdioTransport::close*", 0, 1, GetCurrentThreadId());
        if (!fault.available()) {
          std::fputs("Native close symbols unavailable\n", stderr);
          std::_Exit(44);
        }
        fault.arm();
        transport.close();
        fault.disarm();
        if (!fault.rejected()) {
          std::fputs("Native close allocation injection did not fire\n", stderr);
          std::_Exit(45);
        }
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(0), "");
#else
  GTEST_SKIP() << "Native CRT close allocation injection requires the MSVC Debug runtime";
#endif
}

TEST(Stdio, NativeClosePostAllocationFailureWakesPendingRequestAndReleasesOwnership) {
#if defined(_MSC_VER) && defined(_DEBUG)
  EXPECT_EXIT(
      {
        auto storage = std::make_shared<std::pmr::synchronized_pool_resource>();
        const std::weak_ptr<std::pmr::memory_resource> retained = storage;
        auto options = process_info_options();
        ProcessTempDirectory directory;
        const auto marker = directory.path / "request-received";
        options.arguments = std::vector<std::string>({"hang-signal", process_path_text(marker)});
        options.shutdown_timeout = std::chrono::milliseconds(0);
        options.memory_resource = std::move(storage);
        {
          StdioTransport transport(std::move(options));
          auto request = std::async(std::launch::async, [&] {
            RequestOptions request_options;
            request_options.timeout = std::chrono::seconds(10);
            try {
              (void)transport.exchange(make_request(1, "server/discover"), request_options);
              return false;
            } catch (const TransportError& error) {
              return error.kind() == TransportErrorKind::closed;
            } catch (...) {
              return false;
            }
          });
          const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
          while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < ready_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          if (!std::filesystem::exists(marker)) {
            std::fputs("Fixture did not receive the pending request\n", stderr);
            std::_Exit(48);
          }
          mcp_test::AllocationFault fault("*StdioTransport::Impl::schedule_close*", 0, 1, GetCurrentThreadId());
          if (!fault.available()) {
            std::fputs("Close-post symbols unavailable\n", stderr);
            std::_Exit(44);
          }
          fault.arm();
          transport.close();
          fault.disarm();
          if (!fault.rejected()) {
            std::fputs("Close-post allocation injection did not fire\n", stderr);
            std::_Exit(45);
          }
          if (request.wait_for(std::chrono::seconds(2)) != std::future_status::ready || !request.get()) {
            std::fputs("Pending request did not wake with closed transport\n", stderr);
            std::_Exit(46);
          }
        }
        options.memory_resource.reset();
        if (!retained.expired()) {
          std::fputs("Close-post failure retained Impl ownership\n", stderr);
          std::_Exit(47);
        }
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(0), "");
#else
  GTEST_SKIP() << "Native CRT close-post allocation injection requires the MSVC Debug runtime";
#endif
}

namespace {
class DelayedSendResource final : public std::pmr::memory_resource {
  std::mutex mutex_;
  std::condition_variable condition_;
  unsigned large_allocations_ = 0;
  bool armed_ = false, blocked_ = false, released_ = false;

 public:
  void arm() {
    std::lock_guard lock(mutex_);
    armed_ = true;
  }
  bool wait_until_blocked() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2), [&] { return blocked_; });
  }
  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    {
      std::unique_lock lock(mutex_);
      // The unique 32KiB value is copied first into Pending::request,
      // then into send's value argument after pending registration.
      if (armed_ && bytes >= 32768 && ++large_allocations_ == 2) {
        blocked_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_; });
      }
    }
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }
  void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};
}  // namespace
TEST(Stdio, SendBlockedBeforeRegistrationCannotPostAfterCloseDrains) {
  auto storage = std::make_shared<DelayedSendResource>();
  const std::weak_ptr<std::pmr::memory_resource> retained = storage;
  {
    auto options = process_info_options();
    options.arguments = {"tree-leaf"};
    options.shutdown_timeout = std::chrono::milliseconds(0);
    options.memory_resource = storage;
    auto transport = std::make_shared<StdioTransport>(options);
    auto request = make_request(1, "tools/call", {{"name", "held"}, {"arguments", {{"payload", std::string(32768, 'x')}}}});
    storage->arm();
    auto pending = std::async(std::launch::async, [transport, request] {
      RequestOptions budget;
      budget.timeout = std::chrono::seconds(2);
      try {
        (void)transport->exchange(request, budget);
        return false;
      } catch (const TransportError& error) {
        return error.kind() == TransportErrorKind::closed;
      } catch (...) {
        return false;
      }
    });
    struct Release {
      DelayedSendResource& resource;
      ~Release() { resource.release(); }
    } release{*storage};
    const bool blocked = storage->wait_until_blocked();
    EXPECT_TRUE(blocked) << "The allocation gate must reach send after pending registration";
    transport->close();
    storage->release();
    EXPECT_EQ(pending.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(pending.get());
    transport.reset();
  }
  storage.reset();
  EXPECT_TRUE(retained.expired()) << "A late send must not enqueue an owning callback after close finished draining";
}
