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

template <int num_pop_candidates = 2, StickPeriod period = StickPeriod::Fixed>
class StickSwap : public ModeBase<num_pop_candidates> {
    using base_type = ModeBase<num_pop_candidates>;

    struct alignas(build_config::l1_cache_line_size) AlignedIndex {
        std::atomic<std::size_t> value;
    };

    using permutation_type = std::vector<AlignedIndex>;

    struct SharedData : BaseSharedData<num_pop_candidates> {
        permutation_type permutation;

        explicit SharedData(std::size_t num_pqs) : BaseSharedData<num_pop_candidates>{}, permutation(num_pqs) {
            for (std::size_t i = 0; i < num_pqs; ++i) {
                permutation[i].value = i;
            }
        }
    };

    using period_type = StickPeriodState<period>;

   public:
    using config_type = typename base_type::Config;
    using shared_data_type = SharedData;

   private:
    period_type period_{};
    std::size_t offset_{};

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

    template <typename Context>
    void reassign_all(Context& ctx) noexcept {
        for (std::size_t i = 0; i < static_cast<std::size_t>(num_pop_candidates); ++i) {
            swap_assignment(ctx.shared_data().permutation, i);
        }
        period_.renew();
    }

    [[nodiscard]] std::size_t pq_index(permutation_type const& perm, std::size_t index) const noexcept {
        return perm[offset_ + index].value.load(std::memory_order_relaxed);
    }

    template <typename Context>
    [[nodiscard]] typename base_type::index_array pq_indices(Context const& ctx) const noexcept {
        typename base_type::index_array indices{};
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = pq_index(ctx.shared_data().permutation, i);
        }
        return indices;
    }

   protected:
    explicit StickSwap(config_type const& config, shared_data_type& shared_data) noexcept
        : base_type{config.seed, shared_data},
          period_{config.stickiness},
          offset_{this->id() * static_cast<std::size_t>(num_pop_candidates)} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        if (period_.expired()) {
            reassign_all(ctx);
        }
        while (true) {
            auto indices = pq_indices(ctx);
            auto keys = top_keys(ctx, indices);
            auto best_pos = best_position(ctx, keys);
            auto& guard = ctx.pq_guards()[indices[best_pos]];
            if (!guard.try_lock()) {
                reassign_all(ctx);
                continue;
            }
            auto v = guard.pop_locked();
            guard.unlock();
            if (!v) {
                period_.expire();
                return std::nullopt;
            }
            period_.consume();
            return v;
        }
    }

    template <typename Context>
    void push(Context& ctx, typename Context::value_type const& v) {
        if (period_.expired()) {
            reassign_all(ctx);
        }
        auto push_index = this->random_candidate();
        while (true) {
            auto& guard = ctx.pq_guards()[pq_index(ctx.shared_data().permutation, push_index)];
            if (guard.try_push(v)) {
                period_.consume();
                return;
            }
            swap_assignment(ctx.shared_data().permutation, push_index);
        }
    }
};

}  // namespace multiqueue::mode
