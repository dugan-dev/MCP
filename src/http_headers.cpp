#include "http_headers.h"
#include <climits>
#include <openssl/evp.h>
#include <set>

namespace mcp::detail {
std::string lower(std::string_view value) {
  std::string result(value);
  for (char& c : result)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + ('a' - 'A'));
  return result;
}
bool header_token(std::string_view value) noexcept {
  if (value.empty())
    return false;
  for (const unsigned char c : value)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos))
      return false;
  return true;
}
namespace {
bool safe(std::string_view value) {
  for (const unsigned char c : value)
    if ((c < 0x20 && c != '\t') || c > 0x7e)
      return false;
  return true;
}
bool sentinel(std::string_view value) {
  return value.starts_with("=?base64?") && value.ends_with("?=");
}
bool needs_encoding(std::string_view value) {
  return value.empty() || sentinel(value) || !safe(value) || value.front() == ' ' || value.front() == '\t' || value.back() == ' ' ||
         value.back() == '\t';
}
std::string base64(std::string_view value) {
  if (value.size() > static_cast<std::size_t>(INT_MAX / 4 * 3))
    throw std::length_error("Header too large");
  std::string result(4 * ((value.size() + 2) / 3) + 1, '\0');
  const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), reinterpret_cast<const unsigned char*>(value.data()),
                                   static_cast<int>(value.size()));
  result.resize(static_cast<std::size_t>(size));
  return result;
}
struct Binding {
  std::vector<std::string> path;
  std::string name;
  std::string type;
};
void scan(const Json& node, std::vector<std::string> path, bool reachable, std::set<std::string>& names, std::vector<Binding>& bindings) {
  if (!node.is_object())
    return;
  if (node.contains("x-mcp-header")) {
    if (!reachable || path.empty() || !node["x-mcp-header"].is_string() || !header_token(text(node["x-mcp-header"])) || !node.contains("type") ||
        !node["type"].is_string() || (node["type"] != "string" && node["type"] != "integer" && node["type"] != "boolean"))
      throw std::invalid_argument("Invalid x-mcp-header annotation");
    std::string name(text(node["x-mcp-header"]));
    if (!names.insert(lower(name)).second)
      throw std::invalid_argument("Duplicate x-mcp-header name");
    bindings.push_back({path, std::move(name), std::string(text(node["type"]))});
  }
  if (node.contains("properties") && node["properties"].is_object())
    for (auto it = node["properties"].begin(); it != node["properties"].end(); ++it) {
      auto next = path;
      next.emplace_back(it.key().data(), it.key().size());
      scan(it.value(), std::move(next), reachable, names, bindings);
    }
  for (std::string_view key :
       {"items", "prefixItems", "contains", "additionalProperties", "unevaluatedProperties", "unevaluatedItems", "propertyNames", "not", "if", "then",
        "else", "oneOf", "anyOf", "allOf", "$defs", "definitions", "patternProperties", "dependentSchemas"}) {
    if (!node.contains(key))
      continue;
    const auto& sub = node[key];
    const bool map = key == "$defs" || key == "definitions" || key == "patternProperties" || key == "dependentSchemas";
    if (sub.is_array() || (map && sub.is_object()))
      for (const auto& item : sub)
        scan(item, path, false, names, bindings);
    else
      scan(sub, path, false, names, bindings);
  }
}
std::vector<Binding> bindings(const Json& schema) {
  std::vector<Binding> result;
  std::set<std::string> names;
  scan(schema, {}, true, names, result);
  return result;
}
std::string_view name_source(std::string_view method) {
  if (method == "tools/call" || method == "prompts/get")
    return "name";
  if (method == "resources/read")
    return "uri";
  if (method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel")
    return "taskId";
  return {};
}
const Json* argument_at(const Json& request, const std::vector<std::string>& path) {
  if (!request["params"].contains("arguments"))
    return nullptr;
  const Json* current = &request["params"]["arguments"];
  for (const auto& key : path) {
    if (!current->is_object() || !current->contains(key))
      return nullptr;
    current = &(*current)[key];
  }
  return current;
}
std::string primitive(const Json& value) {
  if (value.is_string())
    return std::string(text(value));
  if (value.is_boolean() || value.is_number_integer())
    return serialize_json(value);
  throw RpcError(ErrorCode::invalid_params, "Header parameter must be string, integer or boolean");
}
[[noreturn]] void mismatch(std::string_view header) {
  throw RpcError(ErrorCode::header_mismatch, "HTTP header does not match request body", {{"header", header}});
}
}  // namespace
std::string encode_header(std::string_view value) {
  return needs_encoding(value) ? "=?base64?" + base64(value) + "?=" : std::string(value);
}
std::string decode_header(std::string_view value) {
  if (!safe(value))
    mismatch("encoded value");
  if (!sentinel(value))
    return std::string(value);
  value.remove_prefix(9);
  value.remove_suffix(2);
  if (value.size() > static_cast<std::size_t>(INT_MAX) || value.size() % 4)
    mismatch("base64");
  for (char c : value)
    if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '+' && c != '/' && c != '=')
      mismatch("base64");
  std::string decoded(value.size() / 4 * 3 + 1, '\0');
  int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()), reinterpret_cast<const unsigned char*>(value.data()),
                             static_cast<int>(value.size()));
  if (size < 0)
    mismatch("base64");
  if (value.ends_with("=="))
    size -= 2;
  else if (value.ends_with('='))
    --size;
  if (size < 0)
    mismatch("base64");
  decoded.resize(static_cast<std::size_t>(size));
  if (base64(decoded) != value)
    mismatch("base64");
  try {
    (void)serialize_json(Json(decoded));
  } catch (...) {
    mismatch("UTF-8");
  }
  return decoded;
}
void validate_header_schema(const Json& schema) {
  (void)bindings(schema);
}
Headers request_headers(const Json& request, const Json& schema) {
  validate_request(request);
  const auto method = text(request["method"]);
  Headers result = {{"mcp-protocol-version", std::string(text(request["params"]["_meta"][version_key]))}, {"mcp-method", std::string(method)}};
  if (const auto source = name_source(method); !source.empty()) {
    if (!request["params"].contains(source) || !request["params"][source].is_string())
      throw RpcError(ErrorCode::invalid_params, "Missing request target");
    result["mcp-name"] = encode_header(text(request["params"][source]));
  }
  if (method == "tools/call")
    for (const auto& binding : bindings(schema))
      if (const auto* value = argument_at(request, binding.path); value && !value->is_null())
        result["mcp-param-" + lower(binding.name)] = encode_header(primitive(*value));
  return result;
}
void validate_headers(const Headers& headers, const Json& request, const Json& schema) {
  const auto expected = request_headers(request, schema);
  for (const auto& [key, value] : expected) {
    const auto it = headers.find(key);
    if (it == headers.end())
      mismatch(key);
    if (key == "mcp-name" || key.starts_with("mcp-param-")) {
      if (decode_header(it->second) != decode_header(value))
        mismatch(key);
    } else if (it->second != value)
      mismatch(key);
  }
  if (request["method"] == "tools/call")
    for (const auto& binding : bindings(schema)) {
      const auto key = "mcp-param-" + lower(binding.name);
      if (!expected.contains(key) && headers.contains(key))
        mismatch(key);
    }
}
}  // namespace mcp::detail
