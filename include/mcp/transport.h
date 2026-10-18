#pragma once
#include <mcp/protocol.h>
#include <memory>
namespace mcp {
class Transport {
  public:
    virtual ~Transport() = default;
    virtual Json exchange(const Json &, const RequestOptions &, NotificationHandler = {}) = 0;
    virtual void close() noexcept = 0;
    virtual void set_tool_schema(std::string_view, const Json &) {}
    virtual void set_tool_schemas(const Json &tools) {
        for (const auto &tool : tools)
            set_tool_schema(text(tool["name"]), tool["inputSchema"]);
    }
};
} // namespace mcp
