#pragma once

#include "pcg_random.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>

namespace multiqueue::mode {

struct BaseConfig {
    int seed{1};
};

struct StickyConfig : BaseConfig {
    int stickiness{16};
};

struct SplitStickyConfig : BaseConfig {
    int pop_stickiness{16};
    int push_stickiness{64};
};

template <int NumPopCandidates>
struct BaseSharedData {
    std::atomic_uint id_count{0};

    explicit BaseSharedData(std::size_t /*num_pqs*/) noexcept {
    }

    [[nodiscard]] unsigned next_id() noexcept {
        return id_count.fetch_add(1, std::memory_order_relaxed);
    }
};

template <typename Context, std::size_t N>
[[nodiscard]] std::array<typename Context::key_type, N> top_keys(Context const& ctx,
                                                                 std::array<std::size_t, N> const& indices) noexcept {
    std::array<typename Context::key_type, N> keys{};
    for (std::size_t i = 0; i < N; ++i) {
        keys[i] = ctx.pq_guards()[indices[i]].top_key();
    }
    return keys;
}

template <typename Context, std::size_t N>
[[nodiscard]] std::size_t best_position(Context const& ctx,
                                        std::array<typename Context::key_type, N> const& keys) noexcept {
    std::size_t best = 0;
    for (std::size_t i = 1; i < N; ++i) {
        if (ctx.compare(keys[best], keys[i])) {
            best = i;
        }
    }
    return best;
}

template <typename Context, std::size_t N>
[[nodiscard]] std::size_t worst_position(Context const& ctx,
                                         std::array<typename Context::key_type, N> const& keys) noexcept {
    std::size_t worst = 0;
    for (std::size_t i = 1; i < N; ++i) {
        if (ctx.compare(keys[i], keys[worst])) {
            worst = i;
        }
    }
    return worst;
}

template <typename Context>
[[nodiscard]] std::optional<typename Context::value_type> take_better_than(Context const& ctx,
                                                                           typename Context::guard_type& guard,
                                                                           typename Context::key_type const& key) {
    if (guard.get_pq().empty() || !ctx.compare(key, Context::get_key(guard.get_pq().top()))) {
        return std::nullopt;
    }
    auto v = guard.get_pq().top();
    guard.get_pq().pop();
    guard.popped();
    return v;
}

template <int NumPopCandidates>
class ModeBase {
    static_assert(NumPopCandidates > 0);

    std::uint32_t id_{};
    pcg32 rng_{};

   public:
    using config_type = BaseConfig;
    using shared_data_type = BaseSharedData<NumPopCandidates>;

   protected:
    using index_array = std::array<std::size_t, static_cast<std::size_t>(NumPopCandidates)>;

    explicit ModeBase(config_type const& config, shared_data_type& shared_data) noexcept
        : id_{shared_data.next_id()} {
        auto seq = std::seed_seq{config.seed, static_cast<int>(id_)};
        rng_.seed(seq);
    }

    [[nodiscard]] std::uint32_t id() const noexcept {
        return id_;
    }

    [[nodiscard]] pcg32& rng() noexcept {
        return rng_;
    }

    [[nodiscard]] std::size_t random_index(std::size_t num_pqs) noexcept {
        return std::uniform_int_distribution<std::size_t>{0, num_pqs - 1}(rng_);
    }

    [[nodiscard]] std::size_t random_candidate() noexcept {
        return rng_() % static_cast<std::size_t>(NumPopCandidates);
    }

    [[nodiscard]] index_array sample_indices(std::size_t num_pqs) noexcept {
        index_array indices{};
        for (auto it = indices.begin(); it != indices.end(); ++it) {
            do {
                *it = random_index(num_pqs);
            } while (std::find(indices.begin(), it, *it) != it);
        }
        return indices;
    }

    template <typename Context>
    void push_random(Context& ctx, typename Context::value_type const& v) {
        while (true) {
            auto& guard = ctx.pq_guards()[random_index(ctx.num_pqs())];
            if (guard.try_push(v)) {
                return;
            }
        }
    }
};

template <int NumPopCandidates>
class StickyModeBase : public ModeBase<NumPopCandidates> {
    using base_type = ModeBase<NumPopCandidates>;

    int stickiness_{};
    int remaining_{0};

   public:
    using config_type = StickyConfig;
    using shared_data_type = typename base_type::shared_data_type;

   protected:
    typename base_type::index_array pop_index_{};

    explicit StickyModeBase(config_type const& config, shared_data_type& shared_data) noexcept
        : base_type{config, shared_data}, stickiness_{config.stickiness} {
        assert(stickiness_ > 0);
    }

    [[nodiscard]] int stickiness() const noexcept {
        return stickiness_;
    }

    [[nodiscard]] bool expired() const noexcept {
        return remaining_ <= 0;
    }

    [[nodiscard]] bool is_fresh() const noexcept {
        return remaining_ == stickiness_;
    }

    void renew() noexcept {
        remaining_ = stickiness_;
    }

    void expire() noexcept {
        remaining_ = 0;
    }

    void consume() noexcept {
        --remaining_;
    }

    template <typename Context>
    void reselect(Context const& ctx) noexcept {
        pop_index_ = this->sample_indices(ctx.num_pqs());
        renew();
    }

    template <typename Context>
    void reselect_if_expired(Context const& ctx) noexcept {
        if (expired()) {
            reselect(ctx);
        }
    }

    template <typename Context>
    void replace(Context const& ctx, std::size_t position) noexcept {
        assert(position < pop_index_.size());
        assert(ctx.num_pqs() > pop_index_.size());
        std::size_t index{};
        do {
            index = this->random_index(ctx.num_pqs());
        } while (std::find(pop_index_.begin(), pop_index_.end(), index) != pop_index_.end());
        pop_index_[position] = index;
    }

    template <typename Context>
    void replace_from(Context const& ctx, std::size_t position) noexcept {
        for (auto it = pop_index_.begin() + position; it != pop_index_.end(); ++it) {
            do {
                *it = this->random_index(ctx.num_pqs());
            } while (std::find(pop_index_.begin(), it, *it) != it);
        }
    }
};

}  // namespace multiqueue::mode
