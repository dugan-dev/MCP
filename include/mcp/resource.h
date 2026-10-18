#pragma once
#include <mcp/protocol.h>
namespace mcp {
struct Resource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
    // Additional top-level descriptor fields; use typed members for modeled fields.
    Json metadata = Json::object();
    Json descriptor() const;
};
struct ResourceTemplate {
    std::string uri_template;
    std::string name;
    std::string description;
    std::string mime_type;
    // Additional top-level descriptor fields; use typed members for modeled fields.
    Json metadata = Json::object();
    Json descriptor() const;
};
Json text_resource(std::string_view uri, std::string_view value,
                   std::string_view mime_type = "text/plain");
} // namespace mcp
