#pragma once
#include <mcp/protocol.h>
#include <optional>
namespace mcp {
struct Tool {
    std::string name;
    std::string description;
    Json input_schema = {{"type", "object"}};
    std::optional<Json> output_schema;
    Json annotations = Json::object();
    // Additional top-level descriptor fields, e.g. title/icons/_meta. Set
    // modeled fields such as outputSchema through the typed members above.
    Json metadata = Json::object();
    Json descriptor() const;
};
} // namespace mcp
