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

   private:
    template <typename Context>
    void mark_selection(Context const& ctx) noexcept {
        for (auto i : this->pop_index_) {
            ctx.pq_guards()[i].set_mark(this->id() + 1);
        }
    }

    template <typename Context>
    void replace_and_mark(Context const& ctx, std::size_t pos) noexcept {
        this->replace(ctx, pos);
        ctx.pq_guards()[this->pop_index_[pos]].set_mark(this->id() + 1);
    }

   protected:
    explicit StickMark(config_type const& config, shared_data_type& shared_data) noexcept
        : base_type{config, shared_data} {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& ctx) {
        if (this->expired()) {
            this->reselect(ctx);
            mark_selection(ctx);
        }
        while (true) {
            auto keys = top_keys(ctx, this->pop_index_);
            auto best_pos = best_position(ctx, keys);
            auto& guard = ctx.pq_guards()[this->pop_index_[best_pos]];
            if (!guard.try_lock_if_marked(this->id() + 1)) {
                replace_and_mark(ctx, best_pos);
                continue;
            }
            auto v = guard.pop_locked();
            guard.unlock_marked();
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
        if (this->expired()) {
            this->reselect(ctx);
            mark_selection(ctx);
        }
        auto push_index = this->random_candidate();
        while (true) {
            auto& guard = ctx.pq_guards()[this->pop_index_[push_index]];
            if (guard.try_lock_if_marked(this->id() + 1)) {
                guard.push_locked(v);
                guard.unlock_marked();
                this->consume();
                return;
            }
            replace_and_mark(ctx, push_index);
        }
    }
};

}  // namespace multiqueue::mode
