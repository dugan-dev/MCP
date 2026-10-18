#pragma once
#include <chrono>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace mcp::detail {
// Zero is useful for shutdown grace; request entry points require positive timeouts.
inline std::chrono::steady_clock::time_point request_deadline(std::chrono::milliseconds timeout) {
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    if (timeout < std::chrono::milliseconds::zero())
        throw std::invalid_argument("Timeout must not be negative");
    const auto now = Clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Duration::max());
    if (timeout > maximum)
        return Clock::time_point::max();
    const auto increment = std::chrono::duration_cast<Duration>(timeout);
    if (now.time_since_epoch() > Duration::max() - increment)
        return Clock::time_point::max();
    return now + increment;
}
inline std::chrono::milliseconds remaining_time(std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (now >= deadline)
        return std::chrono::milliseconds::zero();
    using Rep = Clock::duration::rep;
    if constexpr (std::is_integral_v<Rep> && std::is_signed_v<Rep>) {
        const auto begin = now.time_since_epoch().count();
        const auto end = deadline.time_since_epoch().count();
        if (begin < 0 && end > std::numeric_limits<Rep>::max() + begin)
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max());
    }
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}
} // namespace mcp::detail
