#pragma once

#include "multiqueue/modes/common.hpp"

#include <cstddef>
#include <optional>

namespace multiqueue::mode {

// Like Random, but spreads quality over the two drawn pqs: if popping uncovers
// an element that is better than the other candidate's top, that element is
// moved to the other, worse pq, so that a later draw hitting either of them
// finds something good.
template <int num_pop_candidates = 2, bool pop_stale = true>
class RandomMoveWorse : public ModeBase<num_pop_candidates> {
    static_assert(num_pop_candidates == 2);

    using base_type = ModeBase<num_pop_candidates>;

   public:
    using Config = BaseConfig;
    using SharedData = BaseSharedData<num_pop_candidates>;

   protected:
    explicit RandomMoveWorse(Config const& config, SharedData& shared_data) noexcept
        : base_type{config.seed, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        while (true) {
            auto indices = this->sample_indices(ctx.num_pqs());
            auto keys = top_keys(ctx, indices);
            auto best = best_position(ctx, keys);
            auto& guard = ctx.pq_guards()[indices[best]];
            if (!guard.try_lock()) {
                continue;
            }
            if (guard.get_pq().empty()) {
                guard.unlock();
                return std::nullopt;
            }
            if (!pop_stale && Context::get_key(guard.get_pq().top()) != keys[best]) {
                guard.unlock();
                continue;
            }
            auto v = pop_locked(guard);
            auto displaced = take_better_than(ctx, guard, keys[1 - best]);
            guard.unlock();
            if (displaced) {
                auto& other = ctx.pq_guards()[indices[1 - best]];
                if (!try_push(other, *displaced)) {
                    this->push_random(ctx, *displaced);
                }
            }
            return v;
        }
    }

    template <typename Context>
    void push(Context& ctx, typename Context::value_type const& v) {
        this->push_random(ctx, v);
    }
};

}  // namespace multiqueue::mode
