#include <algorithm>
#include <mcp/protocol.h>
#include <mcp/memory.h>
#include <new>
#include <unordered_set>

namespace mcp {
namespace detail {
namespace {
thread_local const JsonStorage* selected_resource = nullptr;
struct alignas(std::max_align_t) Allocation {
  JsonStorage owner;
  std::pmr::memory_resource* resource;
  std::size_t bytes;
  std::size_t alignment;
  void* base;
};
}  // namespace
const JsonStorage* exchange_resource(const JsonStorage* next) noexcept {
  return std::exchange(selected_resource, next);
}
void* allocate_json(std::size_t bytes, std::size_t alignment) {
  const auto actual_alignment = std::max(alignment, alignof(Allocation));
  const auto offset = (sizeof(Allocation) + actual_alignment - 1) & ~(actual_alignment - 1);
  if (bytes > std::numeric_limits<std::size_t>::max() - offset)
    throw std::bad_array_new_length();
  const auto size = offset + bytes;
  // Unscoped allocations own a context without a TLS cache: new JSON can be
  // created after thread-local destruction, during process static teardown.
  JsonStorage owner = selected_resource ? *selected_resource : memory::default_resource();
  auto* resource = owner.get();
  void* base = resource->allocate(size, actual_alignment);
  auto* data = static_cast<std::byte*>(base) + offset;
  ::new (data - sizeof(Allocation)) Allocation{std::move(owner), resource, size, actual_alignment, base};
  return data;
}
void deallocate_json(void* pointer) noexcept {
  if (!pointer)
    return;
  auto* header = reinterpret_cast<Allocation*>(static_cast<std::byte*>(pointer) - sizeof(Allocation));
  const Allocation saved = *header;
  header->~Allocation();
  saved.resource->deallocate(saved.base, saved.bytes, saved.alignment);
}
}  // namespace detail
JsonScope::JsonScope(JsonStorage resource)
    : resource_(resource ? std::move(resource) : memory::default_resource()), previous_resource_(detail::exchange_resource(&resource_)) {}
JsonScope::~JsonScope() {
  detail::exchange_resource(previous_resource_);
}
Json parse_json(std::string_view bytes, std::size_t max_bytes, std::size_t max_depth) {
  if (bytes.size() > max_bytes)
    throw RpcError(ErrorCode::parse_error, "JSON exceeds byte limit");
  std::vector<std::unordered_set<std::string>> keys;
  auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) {
      if (static_cast<std::size_t>(depth) >= max_depth)
        throw RpcError(ErrorCode::parse_error, "JSON exceeds depth limit");
      keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (!keys.back().emplace(text(value)).second)
        throw RpcError(ErrorCode::parse_error, "Duplicate JSON key");
    } else if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end)
      keys.pop_back();
    return true;
  };
  try {
    return Json::parse(bytes.begin(), bytes.end(), callback);
  } catch (const Json::exception&) {
    throw RpcError(ErrorCode::parse_error, "Invalid UTF-8 JSON");
  }
}
std::string serialize_json(const Json& value) {
  const auto bytes = value.dump();
  return {bytes.data(), bytes.size()};
}
std::string_view text(const Json& value) {
  const auto& string = value.get_ref<const String&>();
  return {string.data(), string.size()};
}
}  // namespace mcp
