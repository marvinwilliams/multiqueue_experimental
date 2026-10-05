#include "multiqueue/multiqueue.hpp"

#include "catch2/catch_test_macros.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <type_traits>
#include <vector>

namespace {

struct StubMode {
    struct config_type {
        int seed{1};
    };

    struct shared_data_type {
        explicit shared_data_type(std::size_t /*num_pqs*/) noexcept {
        }
    };

   protected:
    StubMode(config_type const& /*config*/, shared_data_type& /*shared_data*/) noexcept {
    }

    template <typename Context>
    void push(Context& /*ctx*/, typename Context::value_type const& /*v*/) {
    }

    template <typename Context>
    std::optional<typename Context::value_type> try_pop(Context& /*ctx*/) {
        return std::nullopt;
    }
};

struct StubPolicy {
    using mode_type = StubMode;
    static constexpr int pop_tries = 1;
    static constexpr bool scan = false;
};

using mq_type = multiqueue::ValueMultiQueue<unsigned, std::less<>, StubPolicy>;
using config_type = mq_type::config_type;

}  // namespace

static_assert(!std::is_constructible_v<mq_type, int, int>);
static_assert(std::is_constructible_v<mq_type, std::size_t, config_type>);
static_assert(std::is_constructible_v<mq_type, std::size_t, std::size_t, config_type>);
static_assert(std::is_constructible_v<mq_type, mq_type::priority_queue_type*, mq_type::priority_queue_type*>);

TEST_CASE("multiqueue takes a braced config after the number of pqs", "[multiqueue][constructor]") {
    int const seed = 3;
    mq_type mq(16, {seed});
    REQUIRE(mq.num_pqs() == 16);
    REQUIRE(mq.config().seed == seed);
}

TEST_CASE("multiqueue takes an initial capacity only together with a config", "[multiqueue][constructor]") {
    int const seed = 3;
    mq_type mq(16, 4096, {seed});
    REQUIRE(mq.num_pqs() == 16);
    REQUIRE(mq.config().seed == seed);
}

TEST_CASE("multiqueue is built from a range of pqs", "[multiqueue][constructor]") {
    std::vector<mq_type::priority_queue_type> pqs(8);
    mq_type mq(pqs.begin(), pqs.end());
    REQUIRE(mq.num_pqs() == 8);
}
