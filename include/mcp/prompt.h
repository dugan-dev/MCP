#pragma once
#include <mcp/protocol.h>
namespace mcp {
struct Prompt {
    std::string name;
    std::string description;
    Json arguments = Json::array();
    // Additional top-level descriptor fields; use typed members for modeled fields.
    Json metadata = Json::object();
    Json descriptor() const;
};
Json prompt_result(std::string_view value, std::string_view role = "user");
} // namespace mcp
