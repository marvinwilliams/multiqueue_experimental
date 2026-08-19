#pragma once

#include "multiqueue/modes/common.hpp"
#include "multiqueue/modes/stick_swap.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <type_traits>

namespace multiqueue::mode {

// StickSwap with the top keys of the assigned pqs mirrored in the handle.
//
// StickSwap's permutation assigns every pq to exactly one handle, and both pops
// and pushes only ever touch assigned pqs.  While an assignment holds, this
// handle is therefore the only one that can change the top key of any of its
// pqs -- which means it already knows them, and reading them back out of the
// guards is redundant.  A pop then loads the permutation entries and nothing
// else, and the atomic top-key loads happen once per assignment rather than once
// per operation.
//
// The mirror is keyed by pq index and only used for a candidate whose index
// still matches, so a reassignment simply drops it.  It is not proof against a
// pq being traded away and traded back between two operations, which would leave
// a key another handle has changed in the meantime; that costs a candidate
// comparison based on a stale key, never a lost or duplicated element, and a
// mirrored key is rewritten from the guard every time it is used to pop.  A
// mirrored key is never trusted for reporting the queue empty: if the candidate
// picked from the mirror turns out to be drained, the mirror is dropped and the
// selection read again, so an empty result is always based on fresh keys.
//
// `Key` has to be named here because a mode has no access to the queue's types
// outside its operations; it is checked against the context on first use.
template <typename Key, int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickSwapCached : public StickSwap<num_pop_candidates, period> {
    using base_type = StickSwap<num_pop_candidates, period>;
    using key_array = std::array<Key, static_cast<std::size_t>(num_pop_candidates)>;
    using index_array = typename base_type::index_array;

    // No pq is ever assigned this index, so it marks a mirror entry as unusable
    static constexpr std::size_t no_key = std::numeric_limits<std::size_t>::max();

    // The pq each mirrored key was read from, so that a reassignment invalidates it
    index_array mirror_index_{};
    key_array mirror_key_{};

    void drop_mirror() noexcept {
        mirror_index_.fill(no_key);
    }

   public:
    using Config = typename base_type::Config;
    using SharedData = typename base_type::SharedData;

   protected:
    explicit StickSwapCached(Config const& config, SharedData& shared_data) noexcept
        : base_type{config, shared_data} {
        drop_mirror();
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        static_assert(std::is_same_v<Key, typename Context::key_type>,
                      "StickSwapCached must be instantiated with the queue's key type");
        if (this->period_.expired()) {
            this->reassign(ctx);
            drop_mirror();
        }
        while (true) {
            auto indices = this->assigned_pqs(ctx);
            key_array keys{};
            bool from_mirror = false;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (mirror_index_[i] == indices[i]) {
                    keys[i] = mirror_key_[i];
                    from_mirror = true;
                } else {
                    keys[i] = ctx.pq_guards()[indices[i]].top_key();
                    mirror_index_[i] = indices[i];
                    mirror_key_[i] = keys[i];
                }
            }
            auto best = best_position(ctx, keys);
            auto& guard = ctx.pq_guards()[indices[best]];
            if (!guard.try_lock()) {
                this->reassign(ctx);
                drop_mirror();
                continue;
            }
            auto v = pop_locked(guard);
            if (!v) {
                guard.unlock();
                if (from_mirror) {
                    // Never report empty on a key we did not just read
                    drop_mirror();
                    continue;
                }
                this->period_.expire();
                drop_mirror();
                return std::nullopt;
            }
            mirror_key_[best] = guard.top_key();
            guard.unlock();
            this->consume();
            return v;
        }
    }

    template <typename Context>
    void push(Context& ctx, typename Context::value_type const& v) {
        if (this->period_.expired()) {
            this->reassign(ctx);
            drop_mirror();
        }
        auto push_index = this->random_candidate();
        while (true) {
            auto index = this->assigned_pq(ctx.shared_data().permutation, push_index);
            auto& guard = ctx.pq_guards()[index];
            if (guard.try_lock()) {
                push_locked(guard, v);
                // The lock is still held, so this is the key we leave behind
                mirror_index_[push_index] = index;
                mirror_key_[push_index] = guard.top_key();
                guard.unlock();
                this->consume();
                return;
            }
            // Only the contended pq is traded, the stickiness period goes on
            this->swap_assignment(ctx.shared_data().permutation, push_index);
            mirror_index_[push_index] = no_key;
        }
    }
};

}  // namespace multiqueue::mode
