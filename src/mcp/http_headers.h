#pragma once
#include <mcp/http.h>
namespace mcp::detail {
std::string lower(std::string_view);
bool header_token(std::string_view) noexcept;
std::string encode_header(std::string_view);
std::string decode_header(std::string_view);
void validate_header_schema(const Json &);
Headers request_headers(const Json &, const Json &schema = nullptr);
void validate_headers(const Headers &, const Json &, const Json &schema = nullptr);
} // namespace mcp::detail
