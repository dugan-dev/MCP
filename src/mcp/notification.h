#pragma once
#include <mcp/protocol.h>
#include <optional>
namespace mcp::detail {
struct NotificationState {
    bool acknowledged = false;
    Json honored;
    std::optional<double> progress;
};
void validate_notification(const Json &, const Json &request, NotificationState &);
void validate_stream_end(const Json &, const Json &request, const NotificationState &);
} // namespace mcp::detail
