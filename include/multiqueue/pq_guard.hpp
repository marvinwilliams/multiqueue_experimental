/**
******************************************************************************
* @file:   pq_guard.hpp
*
* @author: Marvin Williams
* @date:   2021/11/09 18:05
* @brief:
*******************************************************************************
**/

#pragma once

#include "multiqueue/build_config.hpp"

#include <atomic>
#include <optional>
#include <type_traits>

namespace multiqueue {

template <typename Key, typename Value, typename KeyOfValue, typename PriorityQueue, typename Sentinel>
class alignas(build_config::l1_cache_line_size) PQGuard {
   public:
    using key_type = Key;
    using value_type = Value;
    using priority_queue_type = PriorityQueue;

   private:
    static_assert(std::is_same_v<value_type, typename priority_queue_type::value_type>,
                  "PriorityQueue::value_type must be the same as Value");
    static_assert(std::atomic<key_type>::is_always_lock_free, "std::atomic<key_type> must be lock-free");
    std::atomic<key_type> top_key_ = Sentinel::sentinel();
    std::atomic_uint32_t lock_ = 0;
    priority_queue_type pq_;

   public:
    explicit PQGuard() = default;

    explicit PQGuard(priority_queue_type pq) noexcept : pq_(std::move(pq)) {
    }

    [[nodiscard]] key_type top_key() const noexcept {
        return top_key_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool empty() const noexcept {
        return Sentinel::is_sentinel(top_key());
    }

    void popped() {
        auto key = (pq_.empty() ? Sentinel::sentinel() : KeyOfValue::get(pq_.top()));
        top_key_.store(key, std::memory_order_relaxed);
    }

    void pushed() {
        auto key = KeyOfValue::get(pq_.top());
        if (key != top_key()) {
            top_key_.store(key, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::optional<value_type> pop_locked() {
        if (get_pq().empty()) {
            return std::nullopt;
        }
        auto v = get_pq().top();
        get_pq().pop();
        popped();
        return v;
    }

    void push_locked(value_type const& v) {
        get_pq().push(v);
        pushed();
    }

    bool try_lock() noexcept {
        // Test first to not invalidate the cache line
        return (lock_.load(std::memory_order_relaxed) & 1U) == 0U &&
            (lock_.exchange(1U, std::memory_order_acquire) & 1U) == 0U;
    }

    bool try_lock_if_marked(std::uint32_t mark) noexcept {
        auto current = lock_.load(std::memory_order_relaxed);
        std::uint32_t marked = mark << 1;
        return (current == marked || current == 0U) &&
            lock_.compare_exchange_strong(current, marked | 1U, std::memory_order_acquire, std::memory_order_relaxed);
    }

    [[nodiscard]] bool try_push(value_type const& v) {
        if (!try_lock()) {
            return false;
        }
        push_locked(v);
        unlock();
        return true;
    }

    void unlock() {
        lock_.store(0U, std::memory_order_release);
    }

    void unlock_marked() noexcept {
        lock_.fetch_and(~1U, std::memory_order_release);
    }

    void set_mark(std::uint32_t mark) noexcept {
        auto current = lock_.load(std::memory_order_relaxed);
        auto old_mark = current >> 1;
        while (!lock_.compare_exchange_weak(current, (mark << 1) | (current & 1U), std::memory_order_relaxed)) {
            if ((current >> 1) != old_mark) {
                return;
            }
        }
    }

    priority_queue_type& get_pq() noexcept {
        return pq_;
    }
};

}  // namespace multiqueue
