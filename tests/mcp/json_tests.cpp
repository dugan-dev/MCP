#include <gtest/gtest.h>
#include <mcp/json.h>
#include <mcp/protocol.h>
#include <random>
#include <thread>

using namespace mcp;
TEST(Json, NestedValuesRoundTrip) {
  const auto bytes = R"({"array":[1,-2,3.5,null,true,"unicode \u4e2d\u6587"],"object":{"a":1}})";
  auto value = parse_json(bytes);
  EXPECT_EQ(parse_json(serialize_json(value)), value);
  EXPECT_EQ(text(value["array"][5]), "unicode \xE4\xB8\xAD\xE6\x96\x87");
}
TEST(Json, StringAndViewKeys) {
  Json value = {{"hello", 1}};
  std::string key = "hello";
  EXPECT_TRUE(value.contains(key));
  EXPECT_EQ(value[key], 1);
  value[std::string("new")] = "text";
  EXPECT_EQ(text(value[std::string_view("new")]), "text");
}
TEST(Json, RejectsDuplicateKeys) {
  EXPECT_THROW(parse_json(R"({"x":1,"x":2})"), RpcError);
  EXPECT_THROW(parse_json(R"({"x":1,"\u0078":2})"), RpcError);
  EXPECT_NO_THROW(parse_json(R"({"x":{"x":1},"y":{"x":2}})"));
}
TEST(Json, DepthAndBytesAreBounded) {
  EXPECT_NO_THROW(parse_json("[[1]]", 5, 2));
  EXPECT_THROW(parse_json("[[[1]]]", 7, 2), RpcError);
  EXPECT_THROW(parse_json("[1]", 2), RpcError);
  EXPECT_NO_THROW(parse_json("1", 1, 0));
}
class InvalidJson : public testing::TestWithParam<std::string> {};
TEST_P(InvalidJson, Rejected) {
  EXPECT_THROW(parse_json(GetParam()), RpcError);
}
INSTANTIATE_TEST_SUITE_P(Wire, InvalidJson,
                         testing::Values("", "[", "{}{}", "{\"a\":NaN}", "[1,]", "\"\xff\"", "\"\\uD800\"", "\"\\uDC00\"", "01", "\"bad\nstring\""));
TEST(Json, AllocationAlignmentAndOverflow) {
  struct alignas(256) Value {
    int x;
  };
  JsonAllocator<Value> allocator;
  auto* value = allocator.allocate(2);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(value) % 256, 0U);
  allocator.deallocate(value, 2);
  EXPECT_THROW(allocator.allocate(std::numeric_limits<std::size_t>::max()), std::bad_array_new_length);
}
TEST(Json, LifetimeAcrossScopesAndThreads) {
  Json value;
  {
    JsonScope scope;
    value = {{"text", std::string(4096, 'x')}, {"array", Json::array({1, 2, 3})}};
  }
  std::thread worker([v = std::move(value)]() mutable {
    EXPECT_EQ(text(v["text"]).size(), 4096U);
    v = nullptr;
  });
  worker.join();
}
TEST(Json, RandomTreesRoundTrip) {
  std::mt19937 generator(17);
  for (int i = 0; i < 500; ++i) {
    Json root = Json::array();
    for (int j = 0; j < 20; ++j)
      root.push_back({{"n", generator()}, {"s", std::string(generator() % 128, static_cast<char>('a' + generator() % 26))}});
    ASSERT_EQ(parse_json(serialize_json(root)), root);
  }
}
