#pragma once

#include <nlohmann/json.hpp>
#include <unimem/all.h>

#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mcp {

using JsonStorage = std::shared_ptr<std::pmr::memory_resource>;

namespace detail {

void* allocate_json(std::size_t, std::size_t);
void deallocate_json(void*) noexcept;
const JsonStorage* exchange_resource(const JsonStorage*) noexcept;

}  // namespace detail
class JsonScope final {
 public:
  explicit JsonScope(JsonStorage resource = {});
  ~JsonScope();
  JsonScope(const JsonScope&) = delete;
  JsonScope& operator=(const JsonScope&) = delete;

 private:
  JsonStorage resource_;
  const JsonStorage* previous_resource_;
};

template <class T>
class JsonAllocator {
 public:
  using value_type = T;
  using is_always_equal = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  JsonAllocator() noexcept = default;
  template <class U>
  JsonAllocator(const JsonAllocator<U>&) noexcept {}
  T* allocate(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::bad_array_new_length();
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL > 0
    // MSVC creates iterator proxies inside noexcept string constructors.
    // Keep that debug bookkeeping outside the resource for JSON data.
    if constexpr (std::is_same_v<T, std::_Container_proxy>) {
      return std::allocator<T>{}.allocate(count);
    } else
#endif
    {
      return static_cast<T*>(detail::allocate_json(count * sizeof(T), alignof(T)));
    }
  }
  void deallocate(T* pointer, [[maybe_unused]] std::size_t count) noexcept {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL > 0
    if constexpr (std::is_same_v<T, std::_Container_proxy>) {
      std::allocator<T>{}.deallocate(pointer, count);
    } else
#endif
    {
      detail::deallocate_json(pointer);
    }
  }
  template <class U>
  bool operator==(const JsonAllocator<U>&) const noexcept {
    return true;
  }
};

using String = std::basic_string<char, std::char_traits<char>, JsonAllocator<char>>;

struct StringLess {
  using is_transparent = void;
  bool operator()(std::string_view left, std::string_view right) const noexcept { return left < right; }
};

template <class Key, class Value, class Compare, class Allocator>
using JsonObject = std::map<Key, Value, StringLess, Allocator>;
using Json = nlohmann::basic_json<JsonObject, std::vector, String, bool, std::int64_t, std::uint64_t, double, JsonAllocator, nlohmann::adl_serializer,
                                  std::vector<std::uint8_t, JsonAllocator<std::uint8_t>>>;
Json parse_json(std::string_view bytes, std::size_t max_bytes = 8 * 1024 * 1024, std::size_t max_depth = 128);
std::string serialize_json(const Json& value);
std::string_view text(const Json& value);

}  // namespace mcp
