#include <cstdio>
#include <cstdlib>
#include <mcp/json.h>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
struct ExitLifetime {
  ~ExitLifetime() {
    using namespace mcp;
    // Main-thread TLS has already been destroyed when process statics run.
    // No earlier JSON remains to keep the unscoped cache owner alive.
    auto value = Json{{"payload", std::string(8192, 'd')}, {"sequence", Json::array({4, 5, 6})}};
    auto restored = parse_json(serialize_json(value));
    if (text(restored["payload"]).size() != 8192 || restored["sequence"][2] != 6)
      std::_Exit(1);
    {
      JsonScope scope;
      Json scoped = {{"payload", std::string(4096, 's')}};
      if (text(scoped["payload"]).size() != 4096)
        std::_Exit(2);
    }
    // Confirm unscoped allocations remain valid after a late scope ends.
    restored["sequence"].push_back(7);
    if (restored["sequence"][3] != 7)
      std::_Exit(3);
    std::puts("JSON static teardown allocation passed");
  }
} exit_lifetime;
}  // namespace

int main(int argc, char**) {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
  // The cold case first creates JSON during static teardown.
  if (argc > 1)
    return 0;
  // Initialize the default TLS cache and release every escaping allocation.
  mcp::Json value = {{"payload", std::string(8192, 'm')}};
  if (mcp::text(value["payload"]).size() != 8192)
    std::_Exit(4);
  return 0;
}
