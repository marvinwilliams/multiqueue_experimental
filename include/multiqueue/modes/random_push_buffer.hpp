#pragma once

#include "multiqueue/build_config.hpp"
#include "multiqueue/modes/common.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

namespace multiqueue::mode {

// A handful of slots that elements can be left in for a pq without locking it.
//
// Slots are claimed with a bit in a mask, so there is no head and no tail that
// would have to be reset, and nothing to go wrong when a claim and a drain
// overlap.  A producer touches one cache line -- the masks -- instead of the
// pq's lock and its heap.
template <typename Value, unsigned Capacity>
class alignas(build_config::l1_cache_line_size) PushSlots {
    static_assert(Capacity > 0 && Capacity <= 32, "Capacity must fit into a 32 bit slot mask");

   public:
    using value_type = Value;
    using mask_type = std::uint32_t;

   private:
    static constexpr mask_type all_slots = Capacity == 32 ? ~mask_type{0} : (mask_type{1} << Capacity) - 1;

    [[nodiscard]] static constexpr mask_type bit_of(unsigned slot) noexcept {
        return mask_type{1} << slot;
    }

    [[nodiscard]] static unsigned lowest_slot(mask_type mask) noexcept {
        assert(mask != 0);
#if defined(__GNUC__) || defined(__clang__)
        return static_cast<unsigned>(__builtin_ctz(mask));
#else
        unsigned slot = 0;
        while ((mask & mask_type{1}) == 0) {
            mask = static_cast<mask_type>(mask >> 1U);
            ++slot;
        }
        return slot;
#endif
    }

    // Slots a producer has taken, but which it may still be writing
    std::atomic<mask_type> claimed_{0};
    // Slots whose value is written and may be read
    std::atomic<mask_type> ready_{0};
    std::array<value_type, Capacity> slots_{};

   public:
    // Puts `v` into a free slot and reports whether there was one.  The value is
    // readable once this returns, but its key is not announced anywhere, so the
    // caller still has to publish it.
    [[nodiscard]] bool try_append(value_type const& v) noexcept {
        auto claimed = claimed_.load(std::memory_order_relaxed);
        // At most one attempt per slot: a buffer this contended is not worth
        // fighting over when taking the pq's lock is an option
        for (unsigned attempt = 0; attempt != Capacity; ++attempt) {
            mask_type free_slots = ~claimed & all_slots;
            if (free_slots == 0) {
                return false;
            }
            auto slot = lowest_slot(free_slots);
            // Acquire, so that a slot the drain has just freed is not written
            // while the drain is still reading it
            auto previous = claimed_.fetch_or(bit_of(slot), std::memory_order_acquire);
            if ((previous & bit_of(slot)) == 0) {
                slots_[slot] = v;
                ready_.fetch_or(bit_of(slot), std::memory_order_release);
                return true;
            }
            claimed = previous;
        }
        return false;
    }

    // Hands every value a producer has left here to `f` and frees the slots.
    // Only the handle holding the pq's lock may drain.
    template <typename F>
    void drain(F f) {
        auto ready = ready_.load(std::memory_order_acquire);
        while (ready != 0) {
            auto slot = lowest_slot(ready);
            f(slots_[slot]);
            // Free for reading first and for claiming second, so that a producer
            // cannot take the slot back before we are done with it
            ready_.fetch_and(~bit_of(slot), std::memory_order_release);
            claimed_.fetch_and(~bit_of(slot), std::memory_order_release);
            ready &= ~bit_of(slot);
        }
    }

    // Shows every value that is ready, without taking it.  Only the handle
    // holding the pq's lock may look, as nobody else can rule out a drain.
    template <typename F>
    void peek(F f) const {
        auto ready = ready_.load(std::memory_order_acquire);
        while (ready != 0) {
            auto slot = lowest_slot(ready);
            f(slots_[slot]);
            ready &= ~bit_of(slot);
        }
    }
};

// Random with pushes that do not take a lock.
//
// A push leaves its element in a small buffer belonging to the target pq and
// announces its key in the pq's cached top key.  The element is therefore
// visible to candidate selection immediately even though it is not in the heap
// yet, so the relaxation this costs is small -- unlike a thread-local batch,
// which hides its contents from every other handle until it is flushed.  Whoever
// next locks the pq moves the buffer into the heap before popping.
//
// What this buys is the push path: a fetch_or on one cache line instead of
// acquiring the lock, sifting the heap and releasing the lock.  Pushes are
// typically half of all operations, and this takes the pq's lock out of all of
// them but the ones that find the buffer full.
//
// `Value` has to be named here because a mode's shared data is built before it
// knows the queue's types; it is checked against the context on first use.
template <typename Value, int num_pop_candidates = 2, unsigned push_buffer_size = 4>
class RandomPushBuffer : public ModeBase<num_pop_candidates> {
    using base_type = ModeBase<num_pop_candidates>;
    using slots_type = PushSlots<Value, push_buffer_size>;

   public:
    using Config = BaseConfig;

    struct SharedData : BaseSharedData<num_pop_candidates> {
        // One buffer per pq, each on its own cache line
        std::vector<slots_type> push_slots;

        explicit SharedData(std::size_t num_pqs)
            : BaseSharedData<num_pop_candidates>(num_pqs), push_slots(num_pqs) {
        }
    };

   private:
    // Announces a key that is available in a pq but not in its heap yet.
    template <typename Context>
    static void publish(Context const& ctx, typename Context::guard_type& guard,
                        typename Context::key_type key) noexcept {
        guard.publish_top_key(key, [&ctx](auto const& lhs, auto const& rhs) { return ctx.compare(lhs, rhs); });
    }

    // Moves everything producers left in the buffer into the locked pq.
    template <typename Context>
    static void drain_into(slots_type& slots, typename Context::guard_type& guard) {
        slots.drain([&guard](Value const& value) { guard.get_pq().push(value); });
    }

    // Re-announces the keys of anything left in the buffer, after the cached top
    // key was overwritten from the heap.
    //
    // `popped` and `pushed` store the top key of the heap, which knows nothing
    // about the buffer.  A producer whose slot became ready after our drain but
    // whose announcement landed before that store would have had its key thrown
    // away, leaving its element in the buffer unseen.  The two fences -- this one
    // and the one in `push` -- rule that out: they are ordered against each other,
    // so either the load below sees that producer's slot and we announce its key,
    // or its own announcement happens after our store and stands.
    template <typename Context>
    static void republish(Context const& ctx, typename Context::guard_type& guard, slots_type const& slots) noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        slots.peek([&ctx, &guard](Value const& value) { publish(ctx, guard, Context::get_key(value)); });
    }

   protected:
    explicit RandomPushBuffer(Config const& config, SharedData& shared_data) noexcept
        : base_type{config.seed, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        static_assert(std::is_same_v<Value, typename Context::value_type>,
                      "RandomPushBuffer must be instantiated with the queue's value type");
        while (true) {
            auto indices = this->sample_indices(ctx.num_pqs());
            auto keys = top_keys(ctx, indices);
            auto index = indices[best_position(ctx, keys)];
            auto& guard = ctx.pq_guards()[index];
            if (!guard.try_lock()) {
                continue;
            }
            auto& slots = ctx.shared_data().push_slots[index];
            drain_into<Context>(slots, guard);
            auto v = pop_locked(guard);
            if (v) {
                republish(ctx, guard, slots);
            }
            // Nothing to re-announce when the pq was empty: the drain found
            // nothing, so the cached top key was never overwritten
            guard.unlock();
            return v;
        }
    }

    template <typename Context>
    void push(Context& ctx, typename Context::value_type const& v) {
        while (true) {
            auto index = this->random_index(ctx.num_pqs());
            auto& slots = ctx.shared_data().push_slots[index];
            auto& guard = ctx.pq_guards()[index];
            if (slots.try_append(v)) {
                // Pairs with the fence in `republish`, so that this announcement
                // cannot be lost between a drain and the store that follows it
                std::atomic_thread_fence(std::memory_order_seq_cst);
                publish(ctx, guard, Context::get_key(v));
                return;
            }
            // The buffer is full, so fall back to the pq itself and empty the
            // buffer while the lock is held anyway
            if (!guard.try_lock()) {
                continue;
            }
            drain_into<Context>(slots, guard);
            push_locked(guard, v);
            republish(ctx, guard, slots);
            guard.unlock();
            return;
        }
    }
};

}  // namespace multiqueue::mode
