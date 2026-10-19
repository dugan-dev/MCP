#include <cstdint>
#include <mcp/server.h>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 65536)
    return 0;
  static mcp::Server server;
  auto result = server.dispatch(std::string_view(reinterpret_cast<const char*>(data), size));
  (void)mcp::serialize_json(result);
  return 0;
}
