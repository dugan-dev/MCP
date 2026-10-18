#include <mcp/extensions.h>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace mcp;
using namespace mcp::extensions;
using namespace std::chrono_literals;

namespace {
class SkillPages final : public Transport {
  public:
    std::size_t calls = 0;
    std::size_t pages = 4;
    std::size_t items = 1;
    std::size_t payload_bytes = 0;
    std::chrono::milliseconds delay{0};
    bool repeated_cursor = false;
    bool empty_cursor = false;
    bool invalid_cache = false;
    bool changing_cache = false;
    std::vector<std::chrono::milliseconds> timeouts;
    Json exchange(const Json &request, const RequestOptions &options, NotificationHandler) override {
        ++calls;
        timeouts.push_back(options.timeout);
        if (delay.count())
            std::this_thread::sleep_for(delay);
        Json result = {{"resultType", "complete"}, {"skills", Json::array()},
                       {"ttlMs", changing_cache && calls > 1 ? 1 : 0},
                       {"cacheScope", invalid_cache ? "invalid" : "private"}};
        for (std::size_t index = 0; index < items; ++index)
            result["skills"].push_back({{"uri", "skill://demo/SKILL.md"},
                                        {"payload", std::string(payload_bytes, 'x')}});
        if (calls < pages)
            result["nextCursor"] = empty_cursor ? "" : repeated_cursor ? "same" : std::to_string(calls);
        return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", std::move(result)}};
    }
    void close() noexcept override {}
};
ClientOptions skill_options() {
    ClientOptions options;
    options.capabilities = {{"extensions", skills_capability()}};
    return options;
}
template<class Call> void expect_kind(Call &&call, TransportErrorKind kind) {
    try {
        call();
        FAIL() << "Expected a bounded pagination failure";
    } catch (const TransportError &error) {
        EXPECT_EQ(error.kind(), kind);
    }
}
}

TEST(ReviewPagination, SkillsHonorsPageLimit) {
    auto peer = std::make_shared<SkillPages>();
    auto options = skill_options();
    options.max_pages = 1;
    Client client(peer, options);
    SkillsClient skills(client);
    expect_kind([&] { (void)skills.list(); }, TransportErrorKind::limit);
    EXPECT_EQ(peer->calls, 1u);
}
TEST(ReviewPagination, SkillsHonorsItemLimitAcrossPages) {
    auto peer = std::make_shared<SkillPages>();
    auto options = skill_options();
    options.max_list_items = 2;
    Client client(peer, options);
    SkillsClient skills(client);
    expect_kind([&] { (void)skills.list(); }, TransportErrorKind::limit);
    EXPECT_EQ(peer->calls, 3u);
}
TEST(ReviewPagination, SkillsHonorsByteLimitBeforeGrowingAggregate) {
    auto peer = std::make_shared<SkillPages>();
    peer->payload_bytes = 1024;
    auto options = skill_options();
    options.max_list_bytes = 128;
    Client client(peer, options);
    SkillsClient skills(client);
    expect_kind([&] { (void)skills.list(); }, TransportErrorKind::limit);
    EXPECT_EQ(peer->calls, 1u);
}
TEST(ReviewPagination, SkillsUsesOneAbsoluteDeadlineAcrossAllPages) {
    auto peer = std::make_shared<SkillPages>();
    peer->delay = 12ms;
    Client client(peer, skill_options());
    SkillsClient skills(client);
    RequestOptions options;
    options.timeout = 20ms;
    expect_kind([&] { (void)skills.list(options); }, TransportErrorKind::timeout);
    ASSERT_GE(peer->timeouts.size(), 1u);
    ASSERT_LE(peer->calls, 2u);
    if (peer->timeouts.size() > 1)
        EXPECT_LT(peer->timeouts[1], peer->timeouts[0]);
}
TEST(ReviewPagination, SkillsRejectsBadCursorAndInconsistentCacheMetadata) {
    for (int mode = 0; mode < 4; ++mode) {
        auto peer = std::make_shared<SkillPages>();
        peer->repeated_cursor = mode == 0;
        peer->empty_cursor = mode == 1;
        peer->invalid_cache = mode == 2;
        peer->changing_cache = mode == 3;
        Client client(peer, skill_options());
        SkillsClient skills(client);
        expect_kind([&] { (void)skills.list(); }, TransportErrorKind::invalid_response);
    }
}
TEST(ReviewPagination, SkillsSuccessfulAggregationRetainsFirstPageMetadata) {
    auto peer = std::make_shared<SkillPages>();
    Client client(peer, skill_options());
    SkillsClient skills(client);
    auto result = skills.list();
    EXPECT_EQ(result["skills"].size(), 4u);
    EXPECT_EQ(result["ttlMs"], 0);
    EXPECT_EQ(result["cacheScope"], "private");
    EXPECT_FALSE(result.contains("nextCursor"));
}
