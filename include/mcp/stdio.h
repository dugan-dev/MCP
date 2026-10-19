#pragma once

#include <mcp/server.h>
#include <mcp/transport.h>

#include <istream>
#include <ostream>
#include <map>
#include <vector>

namespace mcp {

struct StdioOptions {
  std::size_t worker_threads = 4;
  bool drain_on_eof = false;
};

void run_stdio(Server&, std::istream&, std::ostream&, StdioOptions = {});
struct ProcessOptions {
  std::string executable;
  std::vector<std::string> arguments;
  Limits limits;
  // JSON allocations retain this optional thread-safe resource.
  JsonStorage memory_resource;
  std::chrono::milliseconds shutdown_timeout{2000};
  std::string working_directory;
  std::map<std::string, std::string, std::less<>> environment;
  bool inherit_environment = true;
};

class StdioTransport final : public Transport {
 public:
  explicit StdioTransport(ProcessOptions);
  ~StdioTransport() override;
  Json exchange(const Json&, const RequestOptions&, NotificationHandler = {}) override;
  void close() noexcept override;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mcp
