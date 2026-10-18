#pragma once
#include <mcp/protocol.h>
namespace mcp::detail {
inline std::string canonical_id(const Json &id) {
    if (id.is_number_float()) {
        const double value = id.get<double>();
        return "n" + (value < 0 ? std::to_string(static_cast<std::int64_t>(value))
                                : std::to_string(static_cast<std::uint64_t>(value)));
    }
    return (id.is_string() ? "s" : "n") + serialize_json(id);
}
} // namespace mcp::detail
