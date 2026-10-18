#include <atomic>
#include <iostream>
#include <mcp/mcp.h>
#include <thread>
#include <unimem/all.h>

namespace {
constexpr bool statistics_enabled = MCP_MEMORY_STATISTICS != 0;
constexpr auto statistics_mode = statistics_enabled
    ? unimem::StatisticsMode::Basic : unimem::StatisticsMode::Disabled;
struct Counts { std::atomic<std::size_t> live{0}, allocations{0}, deallocations{0}; };
class CountingResource final : public std::pmr::memory_resource {
  public:
    explicit CountingResource(Counts &counts) : counts_(counts) {}
  private:
    Counts &counts_;
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto *value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        counts_.live += bytes;
        ++counts_.allocations;
        return value;
    }
    void do_deallocate(void *value, std::size_t bytes, std::size_t alignment) override {
        counts_.live -= bytes;
        ++counts_.deallocations;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }
};
}
int main() {
    auto owner = std::make_shared<unimem::Global>(statistics_mode);
    auto &memory = *owner;
    // The alias owns this exact context even across a shared-library boundary.
    mcp::JsonStorage measured_resource(owner, memory.resource());
    // jsoncons retains its dialect meta-schema per Json specialization.
    {
        mcp::JsonScope scope(measured_resource);
        mcp::Schema warmup(mcp::Json{{"type", "object"}});
    }
    const auto before = memory.statistics();
    {
        mcp::JsonScope workload_scope(measured_resource);
        mcp::Json value;
        {
            mcp::JsonScope scope(measured_resource);
            value = {{"data", std::string(8192, 'x')}, {"array", mcp::Json::array({1, 2, 3})}};
        }
        if (statistics_enabled && memory.statistics().live_bytes <= before.live_bytes)
            return 1;
        std::thread destroy([value = std::move(value)]() mutable { value = nullptr; });
        destroy.join();
        mcp::ServerOptions options;
        options.memory_resource = measured_resource;
        mcp::Server server(std::move(options));
        for (int i = 0; i < 100; ++i) {
            server.add_tool(
                {"test", "", {{"type", "object"}}},
                [](const mcp::Json &, mcp::RequestContext &) { return mcp::text_result("ok"); });
            auto result = server.dispatch(mcp::make_request(i, "tools/call", {{"name", "test"}}));
            if (!result.contains("result"))
                return 2;
        }
    }
    const auto after = memory.statistics();
    if (statistics_enabled) {
        std::cout << "live_before=" << before.live_bytes << ",live_after=" << after.live_bytes
                  << ",allocations=" << after.allocations - before.allocations
                  << ",deallocations=" << after.deallocations - before.deallocations << '\n';
        if (after.live_bytes != before.live_bytes || after.allocations <= before.allocations ||
            after.allocations - before.allocations != after.deallocations - before.deallocations)
            return 3;
    } else if (after.allocations || after.deallocations || after.reallocations ||
               after.live_bytes || after.peak_live_bytes) {
        return 5;
    }
    Counts counts;
    auto resource = std::make_shared<CountingResource>(counts);
    mcp::Json escaped;
    {
        mcp::JsonScope scope(resource);
        escaped = {{"payload", std::string(8192, 'x')}};
    }
    std::thread destroy([value = std::move(escaped)]() mutable { value = nullptr; });
    destroy.join();
    if (counts.live.load() || !counts.allocations.load() ||
        counts.allocations.load() != counts.deallocations.load())
        return 4;
    std::cout << "configured_backend=" << MCP_MEMORY_BACKEND
              << ",statistics=" << (statistics_enabled ? "enabled" : "disabled")
              << ",custom_resource=balanced,cross_thread_destruction=passed\n";
}
