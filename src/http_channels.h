#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace mcp::detail {
// Channel destruction may erase its registry entry, so retain every live owner
// until the registry mutex is released, including on allocation failure.
template <class Key, class Channel, class Compare, class Allocator>
std::vector<std::shared_ptr<Channel>> snapshot_live_channels(std::mutex& mutex,
                                                             const std::map<Key, std::weak_ptr<Channel>, Compare, Allocator>& registry) {
  std::vector<std::shared_ptr<Channel>> snapshot;
  {
    std::lock_guard lock(mutex);
    // Allocate before weak.lock() acquires a possible last strong owner.
    snapshot.reserve(registry.size());
    for (const auto& [id, weak] : registry) {
      (void)id;
      if (auto channel = weak.lock())
        snapshot.push_back(std::move(channel));
    }
  }
  return snapshot;
}

// Shutdown may run after allocation has failed. Keep the registry intact and
// take one live owner at a time; its destruction still occurs after unlocking.
template <class Key, class Channel, class Compare, class Allocator, class Visitor>
void visit_live_channels(std::mutex& mutex, const std::map<Key, std::weak_ptr<Channel>, Compare, Allocator>& registry, Visitor&& visitor) {
  std::optional<Key> cursor;
  for (;;) {
    std::shared_ptr<Channel> channel;
    {
      std::lock_guard lock(mutex);
      auto entry = cursor ? registry.upper_bound(*cursor) : registry.begin();
      for (; entry != registry.end(); ++entry) {
        cursor = entry->first;
        channel = entry->second.lock();
        if (channel)
          break;
      }
    }
    if (!channel)
      return;
    visitor(channel);
  }
}
}  // namespace mcp::detail
