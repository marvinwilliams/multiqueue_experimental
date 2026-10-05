#include "multiqueue/multiqueue.hpp"

#include "multiqueue/modes/random.hpp"
#include "multiqueue/modes/stick_mark.hpp"
#include "multiqueue/modes/stick_random.hpp"
#include "multiqueue/modes/stick_replace.hpp"
#include "multiqueue/modes/stick_swap.hpp"

#include "catch2/catch_template_test_macros.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/generators/catch_generators.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace {

constexpr int num_pop_candidates = 2;

using ModeRandom = multiqueue::mode::Random<num_pop_candidates>;
using ModeRandomStrict = multiqueue::mode::Random<num_pop_candidates, false>;
using ModeStickRandom = multiqueue::mode::StickRandom<num_pop_candidates>;
using ModeStickMark = multiqueue::mode::StickMark<num_pop_candidates>;
using ModeStickReplace = multiqueue::mode::StickReplace<num_pop_candidates>;
using ModeStickSwap = multiqueue::mode::StickSwap<num_pop_candidates>;

template <typename Mode>
struct TestPolicy {
    using mode_type = Mode;
    static constexpr int pop_tries = 1;
    static constexpr bool scan = true;
};

template <typename Mode>
using test_mq = multiqueue::ValueMultiQueue<unsigned, std::less<>, TestPolicy<Mode>>;

constexpr std::size_t pqs_for(std::size_t num_handles) {
    return num_handles * static_cast<std::size_t>(num_pop_candidates);
}

}  // namespace

#define MODE_LIST ModeRandom, ModeRandomStrict, ModeStickRandom, ModeStickMark, ModeStickReplace, ModeStickSwap

TEMPLATE_TEST_CASE("mode pops every pushed element exactly once", "[modes][conservation]", MODE_LIST) {
    // One handle on as many pqs as it may have, and one on a queue wider than
    // its selection, which exercises reselection and replacement
    std::size_t const num_pqs = GENERATE(pqs_for(1), pqs_for(8));
    CAPTURE(num_pqs);
    static constexpr unsigned num_elements = 5000;

    test_mq<TestType> mq(num_pqs);
    auto handle = mq.get_handle();
    for (unsigned i = 1; i <= num_elements; ++i) {
        handle.push(i);
    }

    std::vector<int> seen(num_elements + 1, 0);
    unsigned popped = 0;
    while (auto v = handle.try_pop()) {
        REQUIRE(*v >= 1);
        REQUIRE(*v <= num_elements);
        ++seen[*v];
        ++popped;
    }
    REQUIRE(popped == num_elements);
    REQUIRE(std::all_of(seen.begin() + 1, seen.end(), [](int c) { return c == 1; }));
}

TEMPLATE_TEST_CASE("mode reports empty on a fresh queue", "[modes][empty]", MODE_LIST) {
    test_mq<TestType> mq(pqs_for(1));
    auto handle = mq.get_handle();
    REQUIRE_FALSE(handle.try_pop().has_value());
}

TEMPLATE_TEST_CASE("mode loses no elements under concurrent push and pop", "[modes][concurrent]", MODE_LIST) {
    static constexpr int num_threads = 4;
    static constexpr unsigned per_thread = 2000;
    static constexpr unsigned num_elements = num_threads * per_thread;

    test_mq<TestType> mq(pqs_for(num_threads));
    std::vector<std::atomic_int> seen(num_elements + 1);
    std::atomic<unsigned> remaining{num_elements};
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&mq, &seen, &remaining, t] {
            auto handle = mq.get_handle();
            for (unsigned i = 0; i < per_thread; ++i) {
                handle.push(static_cast<unsigned>(t) * per_thread + i + 1);
            }
            while (remaining.load(std::memory_order_relaxed) > 0) {
                if (auto v = handle.try_pop()) {
                    seen[*v].fetch_add(1, std::memory_order_relaxed);
                    remaining.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::size_t missing = 0;
    std::size_t duplicated = 0;
    for (unsigned i = 1; i <= num_elements; ++i) {
        int const count = seen[i].load(std::memory_order_relaxed);
        if (count == 0) {
            ++missing;
        } else if (count > 1) {
            ++duplicated;
        }
    }
    REQUIRE(missing == 0);
    REQUIRE(duplicated == 0);
}

TEST_CASE("sticky config reaches the mode", "[modes][config]") {
    using mq_type = test_mq<ModeStickRandom>;
    mq_type::config_type config{};
    config.seed = 7;
    config.stickiness = 3;
    mq_type mq(pqs_for(1), config);
    REQUIRE(mq.config().seed == 7);
    REQUIRE(mq.config().stickiness == 3);
}
