#pragma once

#include <map>
#include <memory_resource>
#include <set>
#include <string>
#include <vector>

namespace mcp::detail {

using String = std::pmr::string;

template <class T>
using Vector = std::pmr::vector<T>;

template <class K, class V, class Compare = std::less<>>
using Map = std::pmr::map<K, V, Compare>;

template <class T, class Compare = std::less<>>
using Set = std::pmr::set<T, Compare>;

}  // namespace mcp::detail
