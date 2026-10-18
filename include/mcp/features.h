#pragma once
#include <mcp/client.h>
#include <mcp/server.h>

namespace mcp {
enum class LogLevel { debug, info, notice, warning, error, critical, alert, emergency };
std::string_view log_level_name(LogLevel) noexcept;
LogLevel parse_log_level(std::string_view);
Json elicitation_form(std::string_view message, Json schema);
Json elicitation_url(std::string_view message, std::string_view url);
Json sampling_request(Json parameters);
Json roots_request();
void validate_input_request(const Json &request, const Json &capabilities);
void validate_input_response(const Json &request, const Json &response);
struct InputHandlers {
    InputHandler elicitation;
    InputHandler sampling;
    InputHandler roots;
    bool sampling_tools = false;
    bool sampling_context = false;
    bool elicitation_form_enabled = true;
    bool elicitation_url_enabled = false;
    Json capabilities() const;
    Json operator()(const Json &request) const;
};
} // namespace mcp
