#pragma once

#include "multiqueue/modes/common.hpp"

#include <cstddef>
#include <optional>

namespace multiqueue::mode {

template <int num_pop_candidates = 2, bool pop_stale = true>
class Random : public ModeBase<num_pop_candidates> {
    using base_type = ModeBase<num_pop_candidates>;

   public:
    using config_type = typename base_type::Config;
    using shared_data_type = typename base_type::SharedData;

   protected:
    explicit Random(config_type const& config, shared_data_type& shared_data) noexcept
        : base_type{config.seed, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        while (true) {
            auto indices = this_sample_indices(ctx.num_pqs());
            auto keys = top_keys(ctx, indices);
            auto best_pos = best_position(ctx, keys);
            auto& guard = ctx.pq_guards()[indices[best_pos]];
            if (!guard.try_lock()) {
                continue;
            }
            if (guard.get_pq().empty()) {
                guard.unlock();
                return std::nullopt;
            }
            if (!pop_stale && Context::get_key(guard.get_pq().top()) != keys[best_pos]) {
                guard.unlock();
                continue;
            }
            auto v = guard.pop_locked();
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
