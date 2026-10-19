#pragma once

#include <mcp/protocol.h>

#include <memory>

namespace mcp {

class SseDecoder final {
 public:
  explicit SseDecoder(std::function<void(std::string_view)>, std::size_t max_event_bytes = 8 * 1024 * 1024);
  ~SseDecoder();
  void feed(std::string_view);
  void finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mcp
