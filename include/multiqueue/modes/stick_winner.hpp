#pragma once

#include "multiqueue/modes/common.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace multiqueue::mode {

// Sticks to a selection but rotates it one candidate at a time, giving up the
// candidate that lost the last comparison and keeping the one that won.
//
// Two things motivate this over redrawing the whole selection.  The cheap one is
// that only one pq goes cold per period instead of all of them.  The interesting
// one is quality: the load balancing literature finds that remembering the best
// choice from the previous round helps more than adding an independent choice
// (Mitzenmacher, Prabhakar and Shah, "Load balancing with memory"), so keeping
// the winner should be better than redrawing both, not just cheaper.  It also
// fixes the worst moment of a wholesale redraw, where every candidate is at its
// most stale just before the period ends -- here the selection always contains
// one recently drawn pq.
//
// The rule is self-correcting: whichever pq keeps winning keeps being drained,
// until it stops winning and is rotated out in its turn.
//
// Contention replaces only the pq that was contended, like StickReplace.  A
// drained selection is the one case where everything is redrawn, since the best
// candidate being empty means all of them are.
template <int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickWinner : public StickyModeBase<num_pop_candidates, period> {
    static_assert(num_pop_candidates >= 2, "Keeping the winner needs something to compare it against");

    using base_type = StickyModeBase<num_pop_candidates, period>;

   public:
    using Config = StickyConfig;
    using SharedData = BaseSharedData<num_pop_candidates>;

   private:
    // False until a full selection has been drawn, and again whenever the whole
    // selection turned out to be worthless.
    bool selected_{false};
    // The candidate that lost the last comparison, i.e. the one to rotate out.
    std::size_t loser_{0};

    // Rotates the loser out when the period ends, or draws everything if there is
    // nothing worth keeping.
    template <typename Context>
    void refresh(Context const& ctx) noexcept {
        if (!selected_) {
            this->reselect(ctx);
            selected_ = true;
        } else if (this->period_.expired()) {
            this->replace(ctx, loser_);
            this->period_.renew();
        }
    }

    // Remembers which candidate to give up next.  Ties would otherwise always
    // name position 0, which is also the winner when all keys are equal, so the
    // loser is moved off the winner in that case.
    template <typename Context, std::size_t N>
    void note_loser(Context const& ctx, std::array<typename Context::key_type, N> const& keys,
                    std::size_t best) noexcept {
        loser_ = worst_position(ctx, keys);
        if (loser_ == best) {
            loser_ = (best + 1) % N;
        }
    }

   protected:
    explicit StickWinner(Config const& config, SharedData& shared_data) noexcept
        : base_type{config.seed, config.stickiness, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        refresh(ctx);
        while (true) {
            auto keys = top_keys(ctx, this->pop_index_);
            auto best = best_position(ctx, keys);
            note_loser(ctx, keys, best);
            auto& guard = ctx.pq_guards()[this->pop_index_[best]];
            if (!guard.try_lock()) {
                this->replace(ctx, best);
                continue;
            }
            auto v = pop_locked(guard);
            guard.unlock();
            if (!v) {
                // Every candidate is drained, so there is no winner to keep
                selected_ = false;
                this->period_.expire();
                return std::nullopt;
            }
            this->consume();
            return v;
        }
    }

    template <typename Context>
    void push(Context& ctx, typename Context::value_type const& v) {
        refresh(ctx);
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
