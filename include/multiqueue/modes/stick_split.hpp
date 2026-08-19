#pragma once

#include "multiqueue/modes/common.hpp"

#include <cstddef>
#include <optional>

namespace multiqueue::mode {

// Sticks separately for popping and pushing, because the two want opposite
// things from a selection.
//
// A pop takes the best of its candidates, so it wants a fresh and diverse view:
// the longer it keeps the same candidates, the longer the global best element
// can sit in a pq nobody looks at, which is what stickiness costs in quality.  A
// push only needs its targets to be uniformly distributed *in the long run* --
// nothing about a single push is improved by drawing its target anew -- so it
// can keep a target far longer at almost no cost in quality, and every push it
// serves from the same pq is one that finds the pq's lock and heap in cache.
//
// Sharing one selection and one period between both, as the other sticky modes
// do, forces a single compromise on those two different problems.  Here the pop
// candidates and the push target are independent, with a period each.  The push
// target is drawn from all pqs rather than from the pop candidates, so that
// pushes stay uniform even while the pop side keeps a narrow view.
template <int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickSplit : public StickyModeBase<num_pop_candidates, period> {
    using base_type = StickyModeBase<num_pop_candidates, period>;

   public:
    using Config = SplitStickyConfig;
    using SharedData = BaseSharedData<num_pop_candidates>;

   private:
    std::size_t push_index_{};
    typename base_type::period_type push_period_{};

    // Draws a new push target and restarts its period.
    template <typename Context>
    void reselect_push(Context const& ctx) noexcept {
        push_index_ = this->random_index(ctx.num_pqs());
        push_period_.renew();
    }

   protected:
    explicit StickSplit(Config const& config, SharedData& shared_data) noexcept
        : base_type{config.seed, config.pop_stickiness, shared_data}, push_period_{config.push_stickiness} {
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
        if (push_period_.expired()) {
            reselect_push(ctx);
        }
        while (true) {
            if (try_push(ctx.pq_guards()[push_index_], v)) {
                push_period_.consume(this->rng());
                return;
            }
            // A contended target has to be given up, so the period starts over
            reselect_push(ctx);
        }
    }
};

}  // namespace multiqueue::mode
