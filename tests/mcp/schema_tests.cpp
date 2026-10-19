#include <fstream>
#include <gtest/gtest.h>
#include <mcp/schema.h>
#include <mcp/server.h>
#include <sstream>

using namespace mcp;
TEST(Schema, TypesAndBounds) {
  Schema schema({{"type", "object"},
                 {"properties", {{"x", {{"type", "integer"}, {"minimum", 2}, {"maximum", 5}}}}},
                 {"required", {"x"}},
                 {"additionalProperties", false}});
  EXPECT_TRUE(schema.accepts({{"x", 3}}));
  EXPECT_FALSE(schema.accepts({{"x", 1}}));
  EXPECT_FALSE(schema.accepts({{"x", 3.1}}));
  EXPECT_FALSE(schema.accepts({{"x", 3}, {"y", 1}}));
}
TEST(Schema, Draft202012PrefixItemsAndUnevaluatedProperties) {
  Schema tuple({{"type", "array"}, {"prefixItems", {{{"type", "integer"}}, {{"type", "string"}}}}, {"items", false}});
  EXPECT_TRUE(tuple.accepts(Json::array({1, "x"})));
  EXPECT_FALSE(tuple.accepts(Json::array({"x", 1})));
  EXPECT_FALSE(tuple.accepts(Json::array({1, "x", 2})));
  Schema object({{"allOf", {{{"properties", {{"a", {{"type", "integer"}}}}}}}}, {"unevaluatedProperties", false}});
  EXPECT_TRUE(object.accepts({{"a", 2}}));
  EXPECT_FALSE(object.accepts({{"a", 2}, {"b", 3}}));
}
TEST(Schema, LocalReferencesAndComposition) {
  Schema schema({{"$defs", {{"small", {{"type", "integer"}, {"maximum", 10}}}}}, {"anyOf", {{{"$ref", "#/$defs/small"}}, {{"type", "string"}}}}});
  EXPECT_TRUE(schema.accepts(3));
  EXPECT_TRUE(schema.accepts("hello"));
  EXPECT_FALSE(schema.accepts(11));
}
TEST(Schema, BooleanSchemas) {
  EXPECT_TRUE(Schema(true).accepts(nullptr));
  EXPECT_FALSE(Schema(false).accepts(1));
}
TEST(Schema, NonDescendingReferenceCyclesAreRejected) {
  EXPECT_THROW(Schema(parse_json(R"({"$ref":"#"})")), std::invalid_argument);
  EXPECT_THROW(Schema(parse_json(R"({"$defs":{"a":{"$ref":"#/$defs/b"},"b":{"$ref":"#/$defs/a"}},"$ref":"#/$defs/a"})")), std::invalid_argument);
  EXPECT_THROW(Schema(parse_json(R"({"allOf":[{"$ref":"#"}]})")), std::invalid_argument);
  EXPECT_THROW(Schema(parse_json(R"({"$dynamicAnchor":"loop","$dynamicRef":"#loop"})")), std::invalid_argument);
}
TEST(Schema, DescendingRecursionAndReferenceLookingDataAreValid) {
  Schema recursive(parse_json(R"({"type":"object","properties":{"child":{"$ref":"#"}}})"));
  EXPECT_TRUE(recursive.accepts(parse_json(R"({"child":{"child":{}}})")));
  EXPECT_FALSE(recursive.accepts(parse_json(R"({"child":1})")));
  Schema data(parse_json(R"({"type":"object","default":{"$ref":"https://example.com/data"}})"));
  EXPECT_TRUE(data.accepts(Json::object()));
}
TEST(Schema, RejectsExternalReferencesAndUnknownDialect) {
  EXPECT_THROW(Schema(Json{{"$ref", "http://127.0.0.1/schema"}}), std::invalid_argument);
  EXPECT_THROW(Schema(Json{{"$schema", "https://example.com/custom"}}), std::invalid_argument);
  EXPECT_THROW(Schema(Json{{"$ref", "#/$defs/missing"}}), std::invalid_argument);
}
TEST(Schema, PercentEncodedLocalReferencesAndMalformedFragments) {
  Schema encoded(parse_json(R"({"$defs":{"a b":{"type":"integer"}},"$ref":"#/$defs/a%20b"})"));
  EXPECT_TRUE(encoded.accepts(7));
  EXPECT_FALSE(encoded.accepts("seven"));
  EXPECT_THROW(Schema(parse_json(R"({"$defs":{"x":{"type":"string"}},"$ref":"#/$defs/%Q0"})")), std::invalid_argument);
  EXPECT_THROW(Schema(parse_json(R"({"$defs":{"x":{"type":"string"}},"$ref":"#/$defs/%00"})")), std::invalid_argument);
}
TEST(Schema, RejectsInvalidSchemaAndComplexity) {
  EXPECT_ANY_THROW(Schema(Json{{"type", "imaginary"}}));
  EXPECT_ANY_THROW(Schema(Json{{"type", "integer"}, {"minimum", "x"}}));
  EXPECT_THROW(Schema(Json{{"properties", {{"x", {{"type", "string"}}}}}}, {1, 10, 1000}), std::invalid_argument);
}
TEST(Schema, OfficialDiscoveryAndToolFrames) {
  std::ifstream file(std::string(MCP_FIXTURES) + "/mcp-2026-07-28.schema.json");
  ASSERT_TRUE(file);
  std::ostringstream bytes;
  bytes << file.rdbuf();
  auto definition = parse_json(bytes.str(), 1024 * 1024, 128);
  definition["$ref"] = "#/$defs/DiscoverResultResponse";
  Schema discover(definition, {128, 100000, 1024 * 1024});
  Server server;
  auto response = server.dispatch(make_request(1, "server/discover"));
  EXPECT_TRUE(discover.accepts(response));
  definition["$ref"] = "#/$defs/ListToolsResultResponse";
  Schema listing(definition, {128, 100000, 1024 * 1024});
  EXPECT_TRUE(listing.accepts(server.dispatch(make_request(2, "tools/list"))));
  response["result"].erase("ttlMs");
  EXPECT_FALSE(discover.accepts(response));
}
