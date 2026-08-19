#pragma once

#include "multiqueue/modes/common.hpp"

#include <cstddef>
#include <optional>

namespace multiqueue::mode {

// Draws `num_pop_candidates` pqs for every pop and takes from the best of them;
// pushes to a uniformly drawn pq.  With `pop_stale` disabled, a pop is retried
// instead of taking an element that arrived after the pq was chosen.
template <int num_pop_candidates = 2, bool pop_stale = true>
class Random : public ModeBase<num_pop_candidates> {
    using base_type = ModeBase<num_pop_candidates>;

   public:
    using Config = BaseConfig;
    using SharedData = BaseSharedData<num_pop_candidates>;

   protected:
    explicit Random(Config const& config, SharedData& shared_data) noexcept : base_type{config.seed, shared_data} {
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
            guard.unlock();
            return v;
        }
    }

    template <typename Context>
    void push(Context& ctx, typename Context::value_type const& v) {
        this->push_random(ctx, v);
    }
};

}  // namespace multiqueue::mode
