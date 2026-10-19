#pragma once

#include <mcp/json.h>

#include <memory>

namespace mcp {

struct SchemaLimits {
  std::size_t depth = 64;
  std::size_t nodes = 2048;
  std::size_t bytes = 256 * 1024;
};

class Schema final {
 public:
  explicit Schema(Json definition, SchemaLimits limits = {});
  const Json& definition() const noexcept;
  bool accepts(const Json&) const;
  void validate(const Json&) const;

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};

}  // namespace mcp
