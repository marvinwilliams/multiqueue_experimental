#pragma once

#include "multiqueue/modes/common.hpp"

#include <cstddef>
#include <optional>

namespace multiqueue::mode {

// StickRandom that marks the pqs it locks with its handle id, so that another
// handle only takes a marked pq away when its own selection is fresh.  This
// keeps two handles from repeatedly stealing the same pq from each other.
template <int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickMark : public StickyModeBase<num_pop_candidates, period> {
    using base_type = StickyModeBase<num_pop_candidates, period>;

   public:
    using Config = StickyConfig;
    using SharedData = BaseSharedData<num_pop_candidates>;

   protected:
    explicit StickMark(Config const& config, SharedData& shared_data) noexcept
        : base_type{config.seed, config.stickiness, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        this->reselect_if_expired(ctx);
        while (true) {
            auto keys = top_keys(ctx, this->pop_index_);
            auto& guard = ctx.pq_guards()[this->pop_index_[best_position(ctx, keys)]];
            // A fresh selection may take a pq marked by another handle
            if (!guard.try_lock(this->period_.is_fresh(), this->id())) {
                this->reselect(ctx);
                continue;
            }
            auto v = pop_locked(guard);
            guard.unlock(this->id());
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
            if (guard.try_lock(this->period_.is_fresh(), this->id())) {
                push_locked(guard, v);
                guard.unlock(this->id());
                this->consume();
                return;
            }
            this->reselect(ctx);
        }
    }
};

}  // namespace multiqueue::mode
