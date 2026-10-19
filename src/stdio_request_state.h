#pragma once
#include <atomic>
#include <stop_token>

namespace mcp::detail {
class StdioRequestState final {
 public:
  // Set under the request-table and output locks. Cooperative stop callbacks
  // run separately, after those locks have been released.
  void mark_cancelled() noexcept { cancelled_.store(true); }
  bool is_cancelled() const noexcept { return cancelled_.load(); }
  // The caller holds the request-table lock across this check and insertion.
  bool can_activate() const noexcept { return !is_cancelled(); }
  std::stop_token stop_token() const noexcept { return stop_.get_token(); }
  void request_stop() noexcept { stop_.request_stop(); }

 private:
  std::atomic<bool> cancelled_{false};
  std::stop_source stop_;
};
}  // namespace mcp::detail
