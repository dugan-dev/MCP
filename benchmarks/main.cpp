#include <mcp/all.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

class LocalTransport final : public mcp::Transport {
 public:
  explicit LocalTransport(mcp::Server& server) : server_(server) {}
  mcp::Json exchange(const mcp::Json& request, const mcp::RequestOptions& options, mcp::NotificationHandler notify) override {
    return server_.dispatch(request, options.stop, std::move(notify));
  }
  void close() noexcept override {}

 private:
  mcp::Server& server_;
};

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::uint64_t
consume(const std::uint64_t* data, std::size_t size) {
  return std::accumulate(data, data + size, std::uint64_t{});
}

template <class Work>
mcp::Json measure(std::string_view name, std::size_t iterations, Work work) {
  std::uint64_t checksum = 0;
  for (std::size_t i = 0; i < 8; ++i)
    checksum += work();
  std::vector<double> samples;
  for (int trial = 0; trial < 7; ++trial) {
    const auto start = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i)
      checksum += work();
    samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count() / iterations);
  }
  std::sort(samples.begin(), samples.end());
  return {{"name", name},
          {"iterations_per_trial", iterations},
          {"trials", samples.size()},
          {"median_ns", samples[samples.size() / 2]},
          {"min_ns", samples.front()},
          {"max_ns", samples.back()},
          {"checksum", checksum},
          {"trial_ns_per_operation", samples}};
}

std::size_t count(std::string_view text) {
  std::size_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < 1 || value > 1000000)
    throw std::invalid_argument("iterations must be an integer from 1 to 1000000");
  return value;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::size_t iterations = 1000;
    std::string output;
    for (int i = 1; i < argc; ++i) {
      const std::string_view argument(argv[i]);
      if (argument == "--iterations" && i + 1 < argc)
        iterations = count(argv[++i]);
      else if (argument == "--output" && i + 1 < argc)
        output = argv[++i];
      else
        throw std::invalid_argument("usage: mcp_benchmark [--iterations N] [--output file.json]");
    }
    auto resource = mcp::memory::default_resource();
    mcp::JsonScope scope(resource);
    const std::string payload = R"({"name":"example","values":[1,2,3,4],"enabled":true})";
    const auto value = mcp::parse_json(payload);
    mcp::Schema schema({{"type", "object"},
                        {"properties", {{"name", {{"type", "string"}}}, {"enabled", {{"type", "boolean"}}}}},
                        {"required", {"name", "enabled"}}});
    mcp::Server server;
    server.add_tool({"echo", "", {{"type", "object"}}},
                    [](const mcp::Json& args, mcp::RequestContext&) { return mcp::text_result(mcp::text(args.at("message"))); });
    mcp::Client local(std::make_shared<LocalTransport>(server));
    mcp::HttpServer http_server(server);
    http_server.start();
    mcp::HttpClientOptions http_options;
    http_options.url = "http://127.0.0.1:" + std::to_string(http_server.port()) + "/mcp";
    mcp::Client http(std::make_shared<mcp::HttpTransport>(std::move(http_options)));
    const mcp::Json args = {{"message", "hello"}};
    mcp::Json results = mcp::Json::array();
    results.push_back(measure("vector/std", iterations, [] {
      std::vector<std::uint64_t> values;
      values.reserve(256);
      for (std::uint64_t i = 0; i < 256; ++i)
        values.push_back(i);
      return consume(values.data(), values.size());
    }));
    results.push_back(measure("vector/pmr-unimemory", iterations, [&] {
      std::pmr::vector<std::uint64_t> values(resource.get());
      values.reserve(256);
      for (std::uint64_t i = 0; i < 256; ++i)
        values.push_back(i);
      return consume(values.data(), values.size());
    }));
    results.push_back(measure("json/parse", iterations, [&] { return mcp::parse_json(payload).size(); }));
    results.push_back(measure("json/serialize", iterations, [&] { return mcp::serialize_json(value).size(); }));
    results.push_back(measure("schema/validate", iterations, [&] { return schema.accepts(value) ? 1 : 0; }));
    results.push_back(measure("rpc/local", iterations, [&] { return local.call_tool("echo", args).at("content").size(); }));
    results.push_back(
        measure("rpc/http-loopback", (std::min)(iterations, std::size_t{1000}), [&] { return http.call_tool("echo", args).at("content").size(); }));
    http.close();
    http_server.stop();
    local.close();
    mcp::Json report = {{"format", 1},
                        {"library_version", mcp::library_version},
                        {"protocol", mcp::protocol_version},
                        {"compiler", MCP_BENCH_COMPILER},
                        {"configuration", MCP_BENCH_CONFIGURATION},
                        {"backend", MCP_BENCH_BACKEND},
                        {"statistics", MCP_BENCH_STATISTICS},
                        {"system", MCP_BENCH_SYSTEM},
                        {"architecture", MCP_BENCH_ARCH},
                        {"linkage", MCP_BENCH_LINKAGE},
                        {"checks", MCP_BENCH_CHECKS},
                        {"dependency_baseline", MCP_BENCH_DEPENDENCIES},
                        {"hardware_threads", std::thread::hardware_concurrency()},
                        {"clock", "steady_clock"},
                        {"payload_bytes", payload.size()},
                        {"results", std::move(results)}};
    const auto text = mcp::serialize_json(report);
    if (output.empty())
      std::cout << text << '\n';
    else {
      std::ofstream file(output, std::ios::binary);
      file.exceptions(std::ios::badbit | std::ios::failbit);
      file << text << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
