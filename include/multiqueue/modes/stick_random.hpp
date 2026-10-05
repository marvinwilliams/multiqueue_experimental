#pragma once

#include "multiqueue/modes/common.hpp"

#include <optional>

namespace multiqueue::mode {

template <int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickRandom : public StickyModeBase<num_pop_candidates, period> {
    using base_type = StickyModeBase<num_pop_candidates, period>;

   public:
    using config_type = typename base_type::Config;
    using shared_data_type = typename base_type::SharedData;

   protected:
    explicit StickRandom(config_type const& config, shared_data_type& shared_data) noexcept
        : base_type{config.seed, config.stickiness, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        this->reselect_if_expired(ctx);
        while (true) {
            auto keys = top_keys(ctx, this->pop_index_);
            auto best_pos = best_position(ctx, keys);
            auto& guard = ctx.pq_guards()[this->pop_index_[best_pos]];
            if (!guard.try_lock()) {
                reselect(ctx);
                continue;
            }
            auto v = guard.pop_locked();
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
            if (guard.try_push(v)) {
                this->consume();
                return;
            }
            this->reselect(ctx);
        }
    }
};

}  // namespace multiqueue::mode
