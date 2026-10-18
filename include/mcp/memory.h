#pragma once
#include <memory>
#include <memory_resource>
#include <unimem/all.h>

namespace mcp::memory {
using Resource = std::shared_ptr<std::pmr::memory_resource>;
// The shared resource owns its allocation context.
inline Resource default_resource() {
    auto memory = std::make_shared<unimem::Global>();
    return Resource(memory, memory->resource());
}
}
