#pragma once

#include "multiqueue/modes/common.hpp"

#include <cstddef>
#include <optional>

namespace multiqueue::mode {

// StickRandom that reacts to a contended pq by replacing only that pq instead of
// drawing a whole new selection.
//
// Contention says that this one pq overlaps with another handle's selection; it
// says nothing about the others, and throwing them away costs the cache lines
// they had warmed up.  Replacing a single candidate is also the smaller
// perturbation for quality: the selection decorrelates gradually rather than
// jumping, so the handle always holds a mix of recently and less recently drawn
// pqs instead of a set that is uniformly stale just before it expires.
//
// A drained pq is different: because the sentinel compares worse than any real
// key, the best candidate being empty means all of them are, so that still ends
// the period and redraws everything.
template <int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickReplace : public StickyModeBase<num_pop_candidates, period> {
    using base_type = StickyModeBase<num_pop_candidates, period>;

   public:
    using Config = StickyConfig;
    using SharedData = BaseSharedData<num_pop_candidates>;

   protected:
    explicit StickReplace(Config const& config, SharedData& shared_data) noexcept
        : base_type{config.seed, config.stickiness, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        this->reselect_if_expired(ctx);
        while (true) {
            auto keys = top_keys(ctx, this->pop_index_);
            auto best = best_position(ctx, keys);
            auto& guard = ctx.pq_guards()[this->pop_index_[best]];
            if (!guard.try_lock()) {
                // Only the pq we collided on moves, the period goes on
                this->replace(ctx, best);
                continue;
            }
            auto v = pop_locked(guard);
            guard.unlock();
            if (!v) {
                this->period_.expire();
                return std::nullopt;
            }
            this->consume();
            return v;
        }
    }

    template <typename Context>
    void push(Context& ctx, typename Context::value_type const& v) {
        this->reselect_if_expired(ctx);
        auto push_index = this->random_candidate();
        while (true) {
            auto& guard = ctx.pq_guards()[this->pop_index_[push_index]];
            if (try_push(guard, v)) {
                this->consume();
                return;
            }
            this->replace(ctx, push_index);
        }
    }
};

}  // namespace multiqueue::mode
