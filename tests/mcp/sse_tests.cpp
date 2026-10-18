#include <gtest/gtest.h>
#include <mcp/sse.h>
using namespace mcp;
TEST(Sse, EverySplitBoundaryIsEquivalent) {
    const std::string bytes =
        "\xEF\xBB\xBF:comment\r\nevent: message\r\ndata: {\"a\":\r\ndata: 1}\r\n\r\ndata: 2\n\n";
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        std::vector<std::string> messages;
        SseDecoder decoder([&](std::string_view value) { messages.emplace_back(value); });
        decoder.feed(std::string_view(bytes).substr(0, split));
        decoder.feed(std::string_view(bytes).substr(split));
        decoder.finish();
        ASSERT_EQ(messages.size(), 2U) << split;
        EXPECT_EQ(messages[0], "{\"a\":\n1}");
        EXPECT_EQ(messages[1], "2");
    }
}
TEST(Sse, ByteByByteAndCrOnly) {
    std::vector<std::string> messages;
    SseDecoder decoder([&](std::string_view value) { messages.emplace_back(value); });
    for (char c : std::string("data: one\r\rdata: two\n\n"))
        decoder.feed(std::string_view(&c, 1));
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[1], "two");
}
TEST(Sse, UnknownFieldsCommentsAndIncompleteEvents) {
    std::vector<std::string> messages;
    SseDecoder decoder([&](std::string_view value) { messages.emplace_back(value); });
    decoder.feed("id: ignored\nretry: 10\n:keepalive\ndata: value\n\ndata: incomplete");
    decoder.finish();
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages[0], "value");
}
TEST(Sse, EventAndLineLimits) {
    SseDecoder line([](std::string_view) {}, 8);
    EXPECT_THROW(line.feed("123456789"), TransportError);
    SseDecoder event([](std::string_view) {}, 10);
    EXPECT_THROW(event.feed("data: aa\ndata: bb\ndata: cc\ndata: dd\n\n"), TransportError);
}
TEST(Sse, EmptyDataAndCustomEvents) {
    std::vector<std::string> messages;
    SseDecoder decoder([&](std::string_view value) { messages.emplace_back(value); });
    decoder.feed("data:\n\nevent: other\ndata: hidden\n\ndata: visible\n\n");
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_TRUE(messages[0].empty());
    EXPECT_EQ(messages[1], "visible");
}
TEST(Sse, CallbackExceptionsPropagate) {
    SseDecoder decoder([](std::string_view) { throw std::runtime_error("stop"); });
    EXPECT_THROW(decoder.feed("data: x\n\n"), std::runtime_error);
}
