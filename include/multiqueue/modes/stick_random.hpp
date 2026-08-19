#pragma once

#include "multiqueue/modes/common.hpp"

#include <cstddef>
#include <optional>

namespace multiqueue::mode {

// Draws `num_pop_candidates` pqs and uses them for a whole stickiness period,
// popping from the best of them and pushing to one of them, which saves the
// draws and keeps the accessed pqs in cache.  A contended pq ends the period and
// the whole selection is drawn again.
template <int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickRandom : public StickyModeBase<num_pop_candidates, period> {
    using base_type = StickyModeBase<num_pop_candidates, period>;

   public:
    using Config = StickyConfig;
    using SharedData = BaseSharedData<num_pop_candidates>;

   protected:
    explicit StickRandom(Config const& config, SharedData& shared_data) noexcept
        : base_type{config.seed, config.stickiness, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        this->reselect_if_expired(ctx);
        while (true) {
            auto keys = top_keys(ctx, this->pop_index_);
            auto& guard = ctx.pq_guards()[this->pop_index_[best_position(ctx, keys)]];
            if (!guard.try_lock()) {
                this->reselect(ctx);
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
            this->reselect(ctx);
        }
    }
};

}  // namespace multiqueue::mode
