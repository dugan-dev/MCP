#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace mcp::detail {
// Releasing a retired Impl may destroy another listener captured by one of
// its callbacks. Its stop must not wait for this same reaper to join it.
inline thread_local bool active_http_shutdown_reaper = false;

// Embedded in the listener. Neither retirement nor joining allocates storage.
struct HttpShutdownThread {
    enum class JoinOwner { none, caller, reaper };
    std::thread thread;
    std::atomic<JoinOwner> join_owner{JoinOwner::none};
    std::atomic<bool> joined{false};
    HttpShutdownThread *registered_next = nullptr, *retired_next = nullptr;
    void *context = nullptr;
    void (*request_stop)(void *) noexcept = nullptr;
    std::shared_ptr<void> retired_owner;
};

// One instance per linked MCP image. It joins an orphaned listener coordinator
// before releasing its last Impl owner, so no running thread is detached.
class HttpShutdownReaper {
  public:
    HttpShutdownReaper() : thread_([this] { run(); }) {}
    ~HttpShutdownReaper() {
        {
            std::lock_guard lock(mutex_);
            closing_.store(true);
            for (auto *record = registered_; record; record = record->registered_next)
                record->request_stop(record->context);
        }
        available_.notify_all();
        thread_.join();
    }
    void add(HttpShutdownThread &record) noexcept {
        std::lock_guard lock(mutex_);
        record.registered_next = registered_;
        registered_ = &record;
        ++count_;
    }
    void remove(HttpShutdownThread &record) noexcept {
        {
            std::lock_guard lock(mutex_);
            auto **entry = &registered_;
            while (*entry != &record)
                entry = &(*entry)->registered_next;
            *entry = record.registered_next;
            --count_;
        }
        record.joined.store(true);
        record.joined.notify_all();
        available_.notify_all();
    }
    void retire(HttpShutdownThread &record, std::shared_ptr<void> owner) noexcept {
        record.retired_owner = std::move(owner);
        {
            std::lock_guard lock(mutex_);
            if (tail_)
                tail_->retired_next = &record;
            else
                head_ = &record;
            tail_ = &record;
        }
        available_.notify_one();
    }

  private:
    std::mutex mutex_;
    std::condition_variable available_;
    std::atomic<bool> closing_{false};
    std::size_t count_ = 0;
    HttpShutdownThread *registered_ = nullptr, *head_ = nullptr, *tail_ = nullptr;
    std::thread thread_;
    void run() noexcept {
        active_http_shutdown_reaper = true;
        for (;;) {
            HttpShutdownThread *record = nullptr;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [&] { return head_ || (closing_.load() && count_ == 0); });
                if (!head_)
                    return;
                record = head_;
                head_ = record->retired_next;
                if (!head_)
                    tail_ = nullptr;
            }
            auto owner = std::move(record->retired_owner);
            record->thread.join();
            remove(*record);
            // owner may destroy the record; do not access it beyond this point.
        }
    }
};

inline HttpShutdownReaper &http_shutdown_reaper() {
    static HttpShutdownReaper reaper;
    return reaper;
}
} // namespace mcp::detail
