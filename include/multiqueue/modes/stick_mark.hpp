#pragma once

#include "multiqueue/modes/common.hpp"

#include <optional>

namespace multiqueue::mode {

template <int num_pop_candidates = 2>
class StickMark : public StickyModeBase<num_pop_candidates> {
    using base_type = StickyModeBase<num_pop_candidates>;

   public:
    using config_type = typename base_type::config_type;
    using shared_data_type = typename base_type::shared_data_type;

   protected:
    explicit StickMark(config_type const& config, shared_data_type& shared_data) noexcept
        : base_type{config, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        this->reselect_if_expired(ctx);
        while (true) {
            auto keys = top_keys(ctx, this->pop_index_);
            auto best_pos = best_position(ctx, keys);
            auto& guard = ctx.pq_guards()[this->pop_index_[best_pos]];
            if (!guard.try_lock(this->is_fresh(), this->id())) {
                this->reselect(ctx);
                continue;
            }
            auto v = guard.pop_locked();
            guard.unlock(this->id());
            if (!v) {
                this->expire();
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
            if (guard.try_lock(this->is_fresh(), this->id())) {
                guard.push_locked(v);
                guard.unlock(this->id());
                this->consume();
                return;
            }
            this->reselect(ctx);
        }
    }
};

}  // namespace multiqueue::mode
