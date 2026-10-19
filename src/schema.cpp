#include <functional>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <map>
#include <mcp/protocol.h>
#include <mcp/schema.h>
#include <set>

namespace mcp {
namespace {
using SchemaJson = jsoncons::basic_json<char, jsoncons::sorted_policy, JsonAllocator<char>>;
SchemaJson convert(const Json& value) {
  if (value.is_object()) {
    SchemaJson result(jsoncons::json_object_arg);
    for (auto it = value.begin(); it != value.end(); ++it)
      result.try_emplace(std::string_view(it.key().data(), it.key().size()), convert(it.value()));
    return result;
  }
  if (value.is_array()) {
    SchemaJson result(jsoncons::json_array_arg);
    result.reserve(value.size());
    for (const auto& item : value)
      result.push_back(convert(item));
    return result;
  }
  if (value.is_string())
    return SchemaJson(text(value));
  if (value.is_boolean())
    return SchemaJson(value.get<bool>());
  if (value.is_number_unsigned())
    return SchemaJson(value.get<std::uint64_t>());
  if (value.is_number_integer())
    return SchemaJson(value.get<std::int64_t>());
  if (value.is_number_float())
    return SchemaJson(value.get<double>());
  return SchemaJson::null();
}
void bounded(const Json& value, std::size_t depth, std::size_t& nodes, const SchemaLimits& limits) {
  if (++nodes > limits.nodes || depth > limits.depth)
    throw std::invalid_argument("Schema exceeds complexity limits");
  if (value.is_structured())
    for (const auto& item : value)
      bounded(item, depth + 1, nodes, limits);
}
// Detect reference cycles that re-evaluate the same instance. Recursion through
// properties/items is valid: those edges descend into a bounded input value.
void safe_references(const Json& root) {
  auto decode_fragment = [](std::string_view fragment) {
    std::string decoded;
    decoded.reserve(fragment.size());
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    for (std::size_t i = 0; i < fragment.size(); ++i) {
      if (fragment[i] != '%') {
        decoded.push_back(fragment[i]);
        continue;
      }
      if (i + 2 >= fragment.size() || hex(fragment[i + 1]) < 0 || hex(fragment[i + 2]) < 0)
        throw std::invalid_argument("Malformed percent encoding in local reference");
      const auto byte = static_cast<char>((hex(fragment[i + 1]) << 4) | hex(fragment[i + 2]));
      if (byte == '\0')
        throw std::invalid_argument("NUL in local reference");
      decoded.push_back(byte);
      i += 2;
    }
    return decoded;
  };
  using Node = const Json*;
  std::map<Node, std::vector<Node>> graph;
  std::map<std::string, std::vector<Node>, std::less<>> anchors;
  std::function<void(const Json&)> collect = [&](const Json& schema) {
    if (!schema.is_object() && !schema.is_boolean())
      return;
    if (!graph.emplace(&schema, std::vector<Node>{}).second || !schema.is_object())
      return;
    for (std::string_view key : {"$anchor", "$dynamicAnchor"})
      if (schema.contains(key) && schema[key].is_string())
        anchors[std::string(text(schema[key]))].push_back(&schema);
    auto child = [&](const Json& value, bool same_instance) {
      collect(value);
      if (same_instance && graph.contains(&value))
        graph[&schema].push_back(&value);
    };
    for (std::string_view key : {"$defs", "definitions", "properties", "patternProperties", "dependentSchemas"})
      if (schema.contains(key) && schema[key].is_object())
        for (const auto& value : schema[key])
          child(value, key == "dependentSchemas");
    for (std::string_view key : {"allOf", "anyOf", "oneOf", "prefixItems"})
      if (schema.contains(key) && schema[key].is_array())
        for (const auto& value : schema[key])
          child(value, key != "prefixItems");
    for (std::string_view key : {"not", "if", "then", "else", "items", "additionalItems", "additionalProperties", "unevaluatedProperties",
                                 "unevaluatedItems", "contains", "propertyNames", "contentSchema"})
      if (schema.contains(key)) {
        const bool same = key == "not" || key == "if" || key == "then" || key == "else";
        if (schema[key].is_array())
          for (const auto& value : schema[key])
            child(value, same);
        else
          child(schema[key], same);
      }
    if (schema.contains("dependencies") && schema["dependencies"].is_object())
      for (const auto& value : schema["dependencies"])
        child(value, true);
  };
  collect(root);
  for (auto& [schema, edges] : graph) {
    if (!schema->is_object())
      continue;
    for (std::string_view key : {"$ref", "$dynamicRef", "$recursiveRef"}) {
      if (!schema->contains(key))
        continue;
      if (!(*schema)[key].is_string() || !text((*schema)[key]).starts_with('#'))
        throw std::invalid_argument("Only local schema references are allowed");
      const auto ref = decode_fragment(text((*schema)[key]).substr(1));
      if (ref.empty())
        edges.push_back(&root);
      else if (ref.starts_with('/')) {
        try {
          const auto* target = &root.at(Json::json_pointer(String(ref.data(), ref.size())));
          if (!graph.contains(target))
            throw std::invalid_argument("Reference does not target a schema");
          edges.push_back(target);
        } catch (const Json::exception&) {
          throw std::invalid_argument("Invalid local schema reference");
        }
      } else {
        const auto it = anchors.find(ref);
        if (it == anchors.end())
          throw std::invalid_argument("Unknown schema anchor");
        edges.insert(edges.end(), it->second.begin(), it->second.end());
      }
    }
  }
  std::map<Node, unsigned> colors;
  std::function<void(Node)> visit = [&](Node node) {
    if (colors[node] == 1)
      throw std::invalid_argument("Schema reference cycle does not descend into input");
    if (colors[node] == 2)
      return;
    colors[node] = 1;
    for (auto next : graph[node])
      visit(next);
    colors[node] = 2;
  };
  for (const auto& [node, edges] : graph) {
    (void)edges;
    visit(node);
  }
}
}  // namespace
struct Schema::Impl {
  Json definition;
  jsoncons::jsonschema::json_schema<SchemaJson> compiled;
  Impl(Json value) : definition(std::move(value)), compiled(jsoncons::jsonschema::make_json_schema(convert(definition))) {}
};
Schema::Schema(Json definition, SchemaLimits limits) {
  if (!definition.is_object() && !definition.is_boolean())
    throw std::invalid_argument("Schema must be an object or boolean");
  std::size_t nodes = 0;
  bounded(definition, 0, nodes, limits);
  if (serialize_json(definition).size() > limits.bytes)
    throw std::invalid_argument("Schema exceeds byte limit");
  safe_references(definition);
  if (definition.is_object()) {
    if (!definition.contains("$schema"))
      definition["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    const auto dialect = text(definition["$schema"]);
    if (dialect != "https://json-schema.org/draft/2020-12/schema" && dialect != "http://json-schema.org/draft-07/schema#" &&
        dialect != "https://json-schema.org/draft-07/schema#")
      throw std::invalid_argument("Unsupported JSON Schema dialect");
    auto meta = jsoncons::jsonschema::meta_resolver<SchemaJson>(jsoncons::uri(std::string(dialect)));
    if (!meta.is_null())
      jsoncons::jsonschema::make_json_schema(std::move(meta)).validate(convert(definition));
  }
  try {
    impl_ = std::make_shared<Impl>(std::move(definition));
  } catch (const std::exception& error) {
    throw std::invalid_argument(std::string("Invalid schema: ") + error.what());
  }
}
const Json& Schema::definition() const noexcept {
  return impl_->definition;
}
bool Schema::accepts(const Json& value) const {
  std::size_t nodes = 0;
  bounded(value, 0, nodes, {128, 1000000, 8 * 1024 * 1024});
  return impl_->compiled.is_valid(convert(value));
}
void Schema::validate(const Json& value) const {
  if (!accepts(value))
    throw RpcError(ErrorCode::invalid_params, "JSON does not satisfy its schema");
}
}  // namespace mcp
