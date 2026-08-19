#pragma once

#include "multiqueue/build_config.hpp"
#include "multiqueue/modes/common.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace multiqueue::mode {

// Sticks to `num_pop_candidates` pqs like StickRandom, but instead of drawing
// them the handles trade them through a shared permutation: reselecting swaps a
// handle's entries with random ones.  Every pq is therefore assigned to exactly
// one handle at a time, so handles never contend for the same pq except through
// a swap in flight.
template <int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickSwap : public ModeBase<num_pop_candidates> {
    using base_type = ModeBase<num_pop_candidates>;

   public:
    struct alignas(build_config::l1_cache_line_size) AlignedIndex {
        std::atomic<std::size_t> value;
    };

    using permutation_type = std::vector<AlignedIndex>;

    using Config = StickyConfig;

    struct SharedData : BaseSharedData<num_pop_candidates> {
        permutation_type permutation;

        explicit SharedData(std::size_t num_pqs)
            : BaseSharedData<num_pop_candidates>(num_pqs), permutation(num_pqs) {
            for (std::size_t i = 0; i < num_pqs; ++i) {
                permutation[i].value = i;
            }
        }
    };

   protected:
    using period_type = StickPeriodState<period>;

    period_type period_{};
    std::size_t offset_{};

    // Trades one of this handle's pqs for a random one.  The entry is parked at
    // `swapping` while the trade is in flight, so that two concurrent swaps
    // cannot duplicate or lose a pq.
    void swap_assignment(permutation_type& perm, std::size_t index) noexcept {
        static constexpr std::size_t swapping = std::numeric_limits<std::size_t>::max();
        assert(index < static_cast<std::size_t>(num_pop_candidates));
        std::size_t old_target = perm[offset_ + index].value.exchange(swapping, std::memory_order_relaxed);
        std::size_t perm_index{};
        std::size_t new_target{};
        do {
            perm_index = this->random_index(perm.size());
            new_target = perm[perm_index].value.load(std::memory_order_relaxed);
        } while (new_target == swapping ||
                 !perm[perm_index].value.compare_exchange_weak(new_target, old_target, std::memory_order_relaxed));
        perm[offset_ + index].value.store(new_target, std::memory_order_relaxed);
    }

    // Trades all of this handle's pqs and restarts its stickiness period.
    template <typename Context>
    void reassign(Context& ctx) noexcept {
        for (std::size_t i = 0; i < static_cast<std::size_t>(num_pop_candidates); ++i) {
            swap_assignment(ctx.shared_data().permutation, i);
        }
        period_.renew();
    }

    [[nodiscard]] std::size_t assigned_pq(permutation_type const& perm, std::size_t index) const noexcept {
        return perm[offset_ + index].value.load(std::memory_order_relaxed);
    }

    // The pqs currently assigned to this handle.
    template <typename Context>
    [[nodiscard]] typename base_type::index_array assigned_pqs(Context const& ctx) const noexcept {
        typename base_type::index_array indices{};
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = assigned_pq(ctx.shared_data().permutation, i);
        }
        return indices;
    }

    // Accounts for one operation served by the current assignment.
    void consume() noexcept {
        period_.consume(this->rng());
    }

    explicit StickSwap(Config const& config, SharedData& shared_data) noexcept
        : base_type{config.seed, shared_data},
          period_{config.stickiness},
          offset_{this->id() * static_cast<std::size_t>(num_pop_candidates)} {
        // Each handle owns num_pop_candidates permutation entries, so the number of
        // handles must not exceed num_pqs / num_pop_candidates
        assert(offset_ + static_cast<std::size_t>(num_pop_candidates) <= shared_data.permutation.size() &&
               "Too many handles: each one needs num_pop_candidates permutation entries");
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        if (period_.expired()) {
            reassign(ctx);
        }
        while (true) {
            auto indices = assigned_pqs(ctx);
            auto keys = top_keys(ctx, indices);
            auto& guard = ctx.pq_guards()[indices[best_position(ctx, keys)]];
            if (!guard.try_lock()) {
                reassign(ctx);
                continue;
            }
            auto v = pop_locked(guard);
            guard.unlock();
            if (!v) {
                period_.expire();
                return std::nullopt;
            }
            consume();
            return v;
        }
    }

    template <typename Context>
    void push(Context& ctx, typename Context::value_type const& v) {
        if (period_.expired()) {
            reassign(ctx);
        }
        auto push_index = this->random_candidate();
        while (true) {
            auto& guard = ctx.pq_guards()[assigned_pq(ctx.shared_data().permutation, push_index)];
            if (try_push(guard, v)) {
                consume();
                return;
            }
            // Only the contended pq is traded, the stickiness period goes on
            swap_assignment(ctx.shared_data().permutation, push_index);
        }
    }
};

}  // namespace multiqueue::mode
