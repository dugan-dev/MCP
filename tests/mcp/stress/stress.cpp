#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <mcp/mcp.h>
#include <vector>

namespace {
std::uint64_t number(const char* s, std::uint64_t fallback) {
  if (!s)
    return fallback;
  std::uint64_t n{};
  const auto end = s + std::char_traits<char>::length(s);
  const auto parsed = std::from_chars(s, end, n);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    throw std::invalid_argument("invalid numeric argument");
  return n;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto seed = number(argc > 1 ? argv[1] : nullptr, 0x4d435053ULL);
    const auto operations = number(argc > 2 ? argv[2] : nullptr, 20000);
    const auto concurrency = number(argc > 3 ? argv[3] : nullptr, 8);
    if (!operations || operations > 10'000'000 || !concurrency || concurrency > 256)
      throw std::invalid_argument("operations must be 1..10000000 and concurrency 1..256");
    std::mt19937_64 rng(seed);
    mcp::ServerOptions options;
    options.limits.message_bytes = 1024 * 1024;
    options.limits.json_depth = 64;
    options.limits.page_size = 7;
    options.max_subscriptions = 1024;
    mcp::Server server(options);
    server.add_tool({"echo", "seeded echo", {{"type", "object"}}},
                    [](const mcp::Json& args, mcp::RequestContext&) { return mcp::text_result(mcp::text(args.at("value"))); });

    // Deterministic property: serializing and parsing generated JSON preserves its value.
    for (std::size_t i = 0; i < 2000; ++i) {
      mcp::Json value = {{"n", rng()},
                         {"text", std::string(rng() % 512, static_cast<char>('a' + rng() % 26))},
                         {"items", mcp::Json::array({rng() % 1000, rng() % 1000, bool(rng() & 1)})}};
      const auto encoded = mcp::serialize_json(value);
      if (mcp::parse_json(encoded, options.limits.message_bytes, options.limits.json_depth) != value)
        throw std::runtime_error("json roundtrip property failed at case " + std::to_string(i));
    }
    // Boundary outcomes are exact and intentionally stable: oversized and malformed input
    // throw.
    bool oversized = false, malformed = false;
    try {
      (void)mcp::parse_json(std::string(options.limits.message_bytes + 1, ' '), options.limits.message_bytes);
    } catch (const mcp::RpcError& e) {
      oversized = e.code() == static_cast<int>(mcp::ErrorCode::parse_error);
    }
    try {
      (void)mcp::parse_json("{ definitely not json");
    } catch (const mcp::RpcError& e) {
      malformed = e.code() == static_cast<int>(mcp::ErrorCode::parse_error);
    }
    bool duplicate_key = false;
    try {
      (void)mcp::parse_json(R"({"jsonrpc":"2.0","id":1,"id":2})");
    } catch (const mcp::RpcError& e) {
      duplicate_key = e.code() == static_cast<int>(mcp::ErrorCode::parse_error);
    }
    if (!oversized || !malformed || !duplicate_key)
      throw std::runtime_error("parser boundary contract failed");

    // Normative envelope mutations cover exact protocol error classes with a reproducible RNG.
    for (std::size_t i = 0; i < 5000; ++i) {
      auto request = mcp::make_request(i + 1, "server/discover");
      const auto mutation = rng() % 4;
      const int expected[] = {-32600, -32602, -32022, -32601};
      if (mutation == 0)
        request["jsonrpc"] = "1.0";
      else if (mutation == 1)
        request["params"].erase("_meta");
      else if (mutation == 2)
        request["params"]["_meta"][mcp::version_key] = "1900-01-01";
      else
        request["method"] = "unknown/method";
      const auto response = server.dispatch(request);
      if (!response.contains("error") || response["error"]["code"] != expected[mutation])
        throw std::runtime_error("seed=" + std::to_string(seed) + " mutation_case=" + std::to_string(i) +
                                 " expected_code=" + std::to_string(expected[mutation]));
    }

    // Input and structured output schemas exercise composition, references, bounds, and
    // rejects.
    const auto alternatives = mcp::Json::array({mcp::Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 9}},
                                                mcp::Json{{"type", "string"}, {"minLength", 2}, {"maxLength", 8}, {"pattern", "^[a-z]+$"}}});
    mcp::Json input_schema = mcp::Json::object();
    input_schema["type"] = "object";
    input_schema["properties"] = {{"value", {{"oneOf", alternatives}}}};
    input_schema["required"] = {"value"};
    input_schema["additionalProperties"] = false;
    server.add_tool(
        {"schema", "schema validation", input_schema,
         mcp::Json{
             {"type", "object"}, {"required", {"accepted"}}, {"properties", {{"accepted", {{"type", "boolean"}}}}}, {"additionalProperties", false}}},
        [](const mcp::Json& args, mcp::RequestContext&) {
          auto result = mcp::text_result("validated");
          result["structuredContent"] = {{"accepted", args.contains("value")}};
          return result;
        });
    const auto call = [&](mcp::Json args) {
      return server.dispatch(mcp::make_request(1, "tools/call", {{"name", "schema"}, {"arguments", std::move(args)}}));
    };
    for (const auto& valid : {mcp::Json{{"value", 1}}, mcp::Json{{"value", 9}}, mcp::Json{{"value", "ab"}}, mcp::Json{{"value", "azerty"}}})
      if (!call(valid).contains("result"))
        throw std::runtime_error("valid schema input rejected");
    for (const auto& invalid : {mcp::Json::object(), mcp::Json{{"value", 0}}, mcp::Json{{"value", 10}}, mcp::Json{{"value", "a"}},
                                mcp::Json{{"value", "UP"}}, mcp::Json{{"value", "123"}}, mcp::Json{{"value", "valid"}, {"extra", true}}}) {
      const auto response = call(invalid);
      if (!response.contains("error") || response["error"]["code"] != -32602)
        throw std::runtime_error("invalid schema input did not return invalid_params");
    }
    server.add_tool({"bad-output", "output schema rejection", {{"type", "object"}}, mcp::Json{{"type", "integer"}}, mcp::Json::object()},
                    [](const mcp::Json&, mcp::RequestContext&) {
                      auto result = mcp::text_result("wrong type");
                      result["structuredContent"] = "wrong";
                      return result;
                    });
    const auto bad_output = server.dispatch(mcp::make_request(1, "tools/call", {{"name", "bad-output"}}));
    if (bad_output["error"]["code"] != -32603)
      throw std::runtime_error("invalid output schema did not return internal_error");

    for (int i = 0; i < 20; ++i)
      server.add_tool({"page-" + std::to_string(i), "pagination fixture", {{"type", "object"}}},
                      [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_result("ok"); });
    auto page_request = mcp::make_request(7, "tools/list");
    std::vector<std::string> page_names;
    for (;;) {
      const auto page = server.dispatch(page_request);
      if (!page.contains("result"))
        throw std::runtime_error("pagination request failed");
      for (const auto& tool : page["result"]["tools"])
        page_names.emplace_back(mcp::text(tool["name"]));
      if (!page["result"].contains("nextCursor"))
        break;
      page_request["params"]["cursor"] = page["result"]["nextCursor"];
    }
    std::sort(page_names.begin(), page_names.end());
    if (page_names.size() != 23 || std::adjacent_find(page_names.begin(), page_names.end()) != page_names.end())
      throw std::runtime_error("pagination lost or duplicated a tool");

    std::atomic<std::uint64_t> completed{0};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (std::uint64_t t = 0; t < concurrency; ++t)
      workers.emplace_back([&, t] {
        try {
          const auto begin = operations * t / concurrency;
          const auto end = operations * (t + 1) / concurrency;
          for (auto i = begin; i < end; ++i) {
            const std::string payload = std::to_string(seed) + ":" + std::to_string(i);
            const auto req = mcp::make_request(i + 1, "tools/call", {{"name", "echo"}, {"arguments", {{"value", payload}}}});
            const auto response = server.dispatch(req);
            if (!response.contains("result") || mcp::text(response["result"]["content"][0]["text"]) != payload)
              throw std::runtime_error("dispatch mismatch at operation " + std::to_string(i));
            if (i % 97 == 0) {
              auto malformed_response = server.dispatch(std::string_view("not-json"));
              if (!malformed_response.contains("error") || malformed_response["error"]["code"] != -32700)
                throw std::runtime_error("malformed RPC did not return parse_error");
            }
            ++completed;
          }
        } catch (const std::exception& e) {
          std::cerr << "seed=" << seed << " thread=" << t << " failure=" << e.what() << '\n';
          failed = true;
        }
      });
    for (auto& worker : workers)
      worker.join();
    if (failed || completed != operations)
      return 1;

    // Concurrent registry snapshots and mutations exercise immutable-entry lifetime and
    // locking.
    workers.clear();
    for (std::uint64_t t = 0; t < concurrency; ++t)
      workers.emplace_back([&, t] {
        try {
          for (std::uint64_t i = 0; i < 500; ++i) {
            const auto name = "churn-" + std::to_string(t) + "-" + std::to_string(i % 8);
            server.add_tool({name, "temporary", {{"type", "object"}}}, [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_result("ok"); });
            const auto listed = server.dispatch(mcp::make_request(i + 1, "tools/list"));
            if (!listed.contains("result") || !listed["result"].contains("tools"))
              throw std::runtime_error("list snapshot failed");
            server.remove_tool(name);
          }
        } catch (const std::exception& e) {
          std::cerr << "seed=" << seed << " registry thread=" << t << " failure=" << e.what() << '\n';
          failed = true;
        }
      });
    for (auto& worker : workers)
      worker.join();
    if (failed)
      return 1;

    // Concurrent acknowledgement, notification delivery, and idempotent close.
    std::atomic<std::uint64_t> notifications{0};
    workers.clear();
    for (std::uint64_t t = 0; t < concurrency; ++t)
      workers.emplace_back([&, t] {
        try {
          for (std::uint64_t i = 0; i < 100; ++i) {
            const auto id = std::to_string(t) + ":" + std::to_string(i);
            std::atomic<std::uint64_t> own_notifications{0};
            auto sub = server.subscribe(mcp::make_request(id, "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}}}}),
                                        [&](const mcp::Json& message) {
                                          if (message.value("method", std::string()) == "notifications/tools/list_changed") {
                                            ++notifications;
                                            ++own_notifications;
                                          }
                                        });
            server.add_tool({"pulse-" + id, "", {{"type", "object"}}}, [](const mcp::Json&, mcp::RequestContext&) { return mcp::text_result("ok"); });
            sub->close();
            sub->close();
            const auto after_close = own_notifications.load();
            server.remove_tool("pulse-" + id);
            if (own_notifications != after_close || after_close == 0)
              throw std::runtime_error("subscription close failed to isolate callback lifetime");
          }
        } catch (const std::exception& e) {
          std::cerr << "seed=" << seed << " subscription thread=" << t << " failure=" << e.what() << '\n';
          failed = true;
        }
      });
    for (auto& worker : workers)
      worker.join();
    if (failed || notifications < concurrency * 100)
      return 1;

    // Cancellation must reach a running handler and produce the documented RPC code.
    std::promise<void> entered;
    server.add_tool({"wait", "", {{"type", "object"}}}, [&](const mcp::Json&, mcp::RequestContext& ctx) {
      entered.set_value();
      while (!ctx.stop_token().stop_requested())
        std::this_thread::yield();
      ctx.throw_if_cancelled();
      return mcp::text_result("unreachable");
    });
    std::stop_source cancellation;
    auto cancelled = std::async(std::launch::async,
                                [&] { return server.dispatch(mcp::make_request(1, "tools/call", {{"name", "wait"}}), cancellation.get_token()); });
    entered.get_future().wait();
    cancellation.request_stop();
    if (cancelled.get()["error"]["code"] != 1001)
      throw std::runtime_error("cancellation outcome mismatch");
    std::cout << "seed=" << seed << " operations=" << completed << " concurrency=" << concurrency << " status=passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
