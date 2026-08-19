/**
******************************************************************************
* @file:   common.hpp
*
* @brief:  Building blocks shared by all modes.
*
* A mode decides which pqs a handle operates on.  It supplies a `Config`, a
* `SharedData` constructible from the number of pqs, a protected constructor
* taking both, and protected `try_pop(ctx)` / `push(ctx, value)`.  Everything a
* mode needs beyond that decision -- seeding its rng, drawing pq indices,
* comparing candidates, operating on a locked pq -- lives here, so that a mode
* header contains only what makes that mode different.
*******************************************************************************
**/
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

// Configuration of modes that draw pqs anew for every operation.
struct BaseConfig {
    int seed{1};
};

// Configuration of modes that stick to the pqs they drew.
struct StickyConfig {
    int seed{1};
    // Number of *additional* operations the selected pqs are used for, i.e. a
    // stickiness of n uses them for n+1 operations and 0 reselects every time.
    int stickiness{16};
};

// Configuration of modes that stick separately for popping and pushing.
struct SplitStickyConfig {
    int seed{1};
    // Additional pops the pop candidates are used for.
    int pop_stickiness{16};
    // Additional pushes the push target is used for.  A push only needs its
    // targets to be uniform in the long run, not per operation, so this may be
    // considerably larger than `pop_stickiness` before quality suffers.
    int push_stickiness{64};
};

// Data all handles of a mode share.  Hands out a distinct id per handle, which
// is what makes their rngs differ.  Modes needing more derive from this.
template <int NumPopCandidates>
struct BaseSharedData {
    std::atomic_uint id_count{0};

    explicit BaseSharedData(std::size_t num_pqs) noexcept {
        assert(num_pqs >= static_cast<std::size_t>(NumPopCandidates) &&
               "Number of pqs must be at least the number of pop candidates");
        (void)num_pqs;
    }

    [[nodiscard]] unsigned next_id() noexcept {
        return id_count.fetch_add(1, std::memory_order_relaxed);
    }
};

// The cached top keys of the given pqs, in the same order.  Read once, so that
// the decisions a mode makes are all based on the same observation.
template <typename Context, std::size_t N>
[[nodiscard]] std::array<typename Context::key_type, N> top_keys(Context const& ctx,
                                                                 std::array<std::size_t, N> const& indices) noexcept {
    std::array<typename Context::key_type, N> keys{};
    for (std::size_t i = 0; i < N; ++i) {
        keys[i] = ctx.pq_guards()[indices[i]].top_key();
    }
    return keys;
}

// Position of the best key, i.e. the candidate to pop from.  A position, not a
// pq index, so that the caller can also address the other candidates.
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

// Position of the worst key, i.e. the candidate a mode gives up first when it
// only wants to part with one of them.
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

// Pops the top element of an already locked pq, or nothing if it is empty.  The
// caller keeps holding the lock and has to release it.
template <typename Guard>
[[nodiscard]] std::optional<typename Guard::value_type> pop_locked(Guard& guard) {
    if (guard.get_pq().empty()) {
        return std::nullopt;
    }
    auto v = guard.get_pq().top();
    guard.get_pq().pop();
    guard.popped();
    return v;
}

// Pushes into an already locked pq.  The caller keeps holding the lock.
template <typename Guard>
void push_locked(Guard& guard, typename Guard::value_type const& v) {
    guard.get_pq().push(v);
    guard.pushed();
}

// Pushes into a pq if it can be locked right away, and reports whether it could.
template <typename Guard>
[[nodiscard]] bool try_push(Guard& guard, typename Guard::value_type const& v) {
    if (!guard.try_lock()) {
        return false;
    }
    push_locked(guard, v);
    guard.unlock();
    return true;
}

// Takes the top out of an already locked pq if it compares better than `key`, so
// that the caller can move it to the worse pq that `key` was read from.  Returns
// nothing if it belongs where it is.  The caller keeps holding the lock.
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

// Base of every mode: owns the handle's rng and knows how to draw pqs from it.
// When to draw is up to the mode.
template <int NumPopCandidates>
class ModeBase {
    static_assert(NumPopCandidates > 0);

    std::uint32_t id_{};
    pcg32 rng_{};

   protected:
    // The pqs a handle considers for a pop.
    using index_array = std::array<std::size_t, static_cast<std::size_t>(NumPopCandidates)>;

    template <typename SharedData>
    explicit ModeBase(int seed, SharedData& shared_data) noexcept : id_{shared_data.next_id()} {
        auto seq = std::seed_seq{seed, static_cast<int>(id_)};
        rng_.seed(seq);
    }

    // Identifies this handle among all handles on the same multiqueue.
    [[nodiscard]] std::uint32_t id() const noexcept {
        return id_;
    }

    // The handle's own rng, for modes that need draws of their own.
    [[nodiscard]] pcg32& rng() noexcept {
        return rng_;
    }

    [[nodiscard]] std::size_t random_index(std::size_t num_pqs) noexcept {
        return std::uniform_int_distribution<std::size_t>{0, num_pqs - 1}(rng_);
    }

    // Position within a selection, e.g. to pick which of the selected pqs to push to.
    [[nodiscard]] std::size_t random_candidate() noexcept {
        return rng_() % static_cast<std::size_t>(NumPopCandidates);
    }

    // Distinct pq indices, drawn by rejection sampling.  Needs at least
    // NumPopCandidates pqs, which BaseSharedData asserts on construction.
    [[nodiscard]] index_array sample_indices(std::size_t num_pqs) noexcept {
        index_array indices{};
        for (auto it = indices.begin(); it != indices.end(); ++it) {
            do {
                *it = random_index(num_pqs);
            } while (std::find(indices.begin(), it, *it) != it);
        }
        return indices;
    }

    // Pushes into a uniformly drawn pq, retrying elsewhere while contended.
    template <typename Context>
    void push_random(Context& ctx, typename Context::value_type const& v) {
        while (true) {
            auto& guard = ctx.pq_guards()[random_index(ctx.num_pqs())];
            if (try_push(guard, v)) {
                return;
            }
        }
    }
};

// How long a handle keeps a selection once it has made one.
enum class StickPeriod {
    // Exactly `stickiness` further operations.  Handles that run at similar
    // speeds then reselect in lockstep, so their reselections -- and the
    // contention those cause -- arrive in bursts.
    Fixed,
    // `stickiness` further operations in expectation, by ending the period after
    // every operation with probability 1/(stickiness+1).  Costs one random draw
    // per operation and makes the reselections of different handles independent.
    Geometric,
};

// Tracks how long the current selection is still to be used.  A stickiness of n
// lets a selection serve n *additional* operations, so n+1 in total, and 0
// reselects on every operation.  With a geometric period that count holds in
// expectation rather than exactly.
template <StickPeriod Period = StickPeriod::Fixed>
class StickPeriodState {
    // Fixed: operations left.  Geometric: 0 while live, -1 once expired.
    int count_{-1};
    bool fresh_{false};
    int stickiness_{};
    // Probability of ending a geometric period after an operation, as the share
    // of the 32-bit draws that fall at or below it.
    std::uint32_t end_threshold_{};

   public:
    StickPeriodState() = default;

    explicit StickPeriodState(int stickiness) noexcept : stickiness_{stickiness} {
        assert(stickiness >= 0 && "Stickiness must not be negative");
        auto period = static_cast<std::uint32_t>(stickiness > 0 ? stickiness : 0) + 1;
        end_threshold_ = std::numeric_limits<std::uint32_t>::max() / period;
    }

    [[nodiscard]] int stickiness() const noexcept {
        return stickiness_;
    }

    // True if the selection is used up and the handle has to select again.
    [[nodiscard]] bool expired() const noexcept {
        return count_ < 0;
    }

    // True if the current selection has not served an operation yet.
    [[nodiscard]] bool is_fresh() const noexcept {
        return fresh_;
    }

    // Begins a period for a selection just made.
    void renew() noexcept {
        if constexpr (Period == StickPeriod::Fixed) {
            count_ = stickiness_;
        } else {
            count_ = 0;
        }
        fresh_ = true;
    }

    // Ends the period, so the next operation selects again.
    void expire() noexcept {
        count_ = -1;
        fresh_ = false;
    }

    // Accounts for one operation served by the current selection.  `rng` is only
    // drawn from for geometric periods.
    template <typename Rng>
    void consume(Rng& rng) noexcept {
        fresh_ = false;
        if constexpr (Period == StickPeriod::Fixed) {
            --count_;
        } else if (rng() <= end_threshold_) {
            count_ = -1;
        }
    }
};

// Base of modes that draw a selection of pqs and then stay with it.  How the
// selection reacts to a contended or drained pq is up to the mode; this only
// provides the selection, the period, and the two ways of changing a selection:
// drawing a whole new one and replacing a single candidate.
template <int NumPopCandidates, StickPeriod Period = StickPeriod::Fixed>
class StickyModeBase : public ModeBase<NumPopCandidates> {
    using base_type = ModeBase<NumPopCandidates>;

   protected:
    using period_type = StickPeriodState<Period>;

    typename base_type::index_array pop_index_{};
    period_type period_{};

    template <typename SharedData>
    explicit StickyModeBase(int seed, int stickiness, SharedData& shared_data) noexcept
        : base_type{seed, shared_data}, period_{stickiness} {
    }

    // Draws a whole new selection and restarts its period.
    template <typename Context>
    void reselect(Context const& ctx) noexcept {
        pop_index_ = this->sample_indices(ctx.num_pqs());
        period_.renew();
    }

    // Draws a selection if the current one is used up.
    template <typename Context>
    void reselect_if_expired(Context const& ctx) noexcept {
        if (period_.expired()) {
            reselect(ctx);
        }
    }

    // Draws a replacement for a single candidate and keeps the others, so that
    // the pqs which are not the problem stay warm.  Leaves the period alone: a
    // mode decides for itself whether swapping one pq restarts it.
    //
    // Does nothing if every pq is already selected, as there is then nothing to
    // replace the candidate with.
    template <typename Context>
    void replace(Context const& ctx, std::size_t position) noexcept {
        assert(position < pop_index_.size());
        if (ctx.num_pqs() <= pop_index_.size()) {
            return;
        }
        std::size_t index{};
        do {
            index = this->random_index(ctx.num_pqs());
        } while (std::find(pop_index_.begin(), pop_index_.end(), index) != pop_index_.end());
        pop_index_[position] = index;
    }

    // Accounts for one operation served by the current selection.
    void consume() noexcept {
        period_.consume(this->rng());
    }
};

}  // namespace multiqueue::mode
