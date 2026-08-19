#include "multiqueue/multiqueue.hpp"

#include "multiqueue/modes/random.hpp"
#include "multiqueue/modes/random_move_worse.hpp"
#include "multiqueue/modes/random_push_buffer.hpp"
#include "multiqueue/modes/stick_mark.hpp"
#include "multiqueue/modes/stick_random.hpp"
#include "multiqueue/modes/stick_random_move_to_worse.hpp"
#include "multiqueue/modes/stick_replace.hpp"
#include "multiqueue/modes/stick_split.hpp"
#include "multiqueue/modes/stick_swap.hpp"
#include "multiqueue/modes/stick_swap_cached.hpp"
#include "multiqueue/modes/stick_winner.hpp"

#include "catch2/catch_template_test_macros.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/generators/catch_generators_all.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <numeric>
#include <thread>
#include <vector>

// Every mode listed here is instantiated and exercised below. Adding a mode to
// this list is what keeps it from silently rotting: the modes are templates, so
// nothing in them is compiled until someone instantiates them.
namespace {

constexpr int num_pop_candidates = 2;
constexpr auto geometric = multiqueue::mode::StickPeriod::Geometric;

// The queue these modes are tested against, so that the modes needing to name
// the queue's types have something to name.
using test_key = unsigned;
using test_value = unsigned;

// Aliased so the type lists below contain no commas inside template arguments,
// which the test macros would split into separate arguments.
using ModeRandom = multiqueue::mode::Random<num_pop_candidates>;
using ModeRandomStrict = multiqueue::mode::Random<num_pop_candidates, false>;
using ModeRandomMoveWorse = multiqueue::mode::RandomMoveWorse<num_pop_candidates>;
using ModeStickRandom = multiqueue::mode::StickRandom<num_pop_candidates>;
using ModeStickRandomMTW = multiqueue::mode::StickRandomMTW<num_pop_candidates>;
using ModeStickMark = multiqueue::mode::StickMark<num_pop_candidates>;
using ModeStickSwap = multiqueue::mode::StickSwap<num_pop_candidates>;
// Replacing a single candidate instead of the whole selection
using ModeStickReplace = multiqueue::mode::StickReplace<num_pop_candidates>;
// Keeping the winner and rotating the loser out
using ModeStickWinner = multiqueue::mode::StickWinner<num_pop_candidates>;
// Mirroring the top keys of the pqs a handle exclusively owns
using ModeStickSwapCached = multiqueue::mode::StickSwapCached<test_key, num_pop_candidates>;
// Pushing without taking a lock
using ModeRandomPushBuffer = multiqueue::mode::RandomPushBuffer<test_value, num_pop_candidates, 4>;
// A push buffer of one slot, so that the full-buffer fallback is taken often
using ModeRandomPushBufferTiny = multiqueue::mode::RandomPushBuffer<test_value, num_pop_candidates, 1>;
// Ending the period with a coin flip rather than a counter
using ModeStickRandomGeom = multiqueue::mode::StickRandom<num_pop_candidates, geometric>;
using ModeStickReplaceGeom = multiqueue::mode::StickReplace<num_pop_candidates, geometric>;
using ModeStickWinnerGeom = multiqueue::mode::StickWinner<num_pop_candidates, geometric>;
using ModeStickSwapGeom = multiqueue::mode::StickSwap<num_pop_candidates, geometric>;
// Separate periods for popping and pushing
using ModeStickSplit = multiqueue::mode::StickSplit<num_pop_candidates>;
using ModeStickSplitGeom = multiqueue::mode::StickSplit<num_pop_candidates, geometric>;

template <typename Mode>
struct TestPolicy {
    using mode_type = Mode;
    static constexpr int pop_tries = 1;
    static constexpr bool scan = true;
};

template <typename Mode>
using test_mq = multiqueue::ValueMultiQueue<test_value, std::less<>, TestPolicy<Mode>>;

// stick_swap assigns each handle num_pop_candidates permutation entries, so the
// number of pqs bounds the number of handles for every mode we test uniformly.
constexpr std::size_t pqs_for(std::size_t num_handles) {
    return num_handles * static_cast<std::size_t>(num_pop_candidates);
}

}  // namespace

#define MODE_LIST                                                                                                    \
    ModeRandom, ModeRandomStrict, ModeRandomMoveWorse, ModeStickRandom, ModeStickRandomMTW, ModeStickMark,            \
        ModeStickSwap, ModeStickReplace, ModeStickWinner, ModeStickSwapCached, ModeRandomPushBuffer,                  \
        ModeRandomPushBufferTiny, ModeStickRandomGeom, ModeStickReplaceGeom, ModeStickWinnerGeom, ModeStickSwapGeom, \
        ModeStickSplit, ModeStickSplitGeom

// Modes configured by a single stickiness, i.e. everything but StickSplit
#define STICKY_MODE_LIST                                                                                       \
    ModeStickRandom, ModeStickRandomMTW, ModeStickMark, ModeStickSwap, ModeStickReplace, ModeStickWinner,      \
        ModeStickSwapCached, ModeStickRandomGeom, ModeStickReplaceGeom, ModeStickWinnerGeom, ModeStickSwapGeom

#define SPLIT_STICKY_MODE_LIST ModeStickSplit, ModeStickSplitGeom

TEMPLATE_TEST_CASE("mode pops every pushed element exactly once", "[modes][conservation]", MODE_LIST) {
    static constexpr unsigned num_elements = 5000;

    test_mq<TestType> mq(pqs_for(1));
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

TEMPLATE_TEST_CASE("mode drains a queue refilled after running empty", "[modes][empty]", MODE_LIST) {
    test_mq<TestType> mq(pqs_for(1));
    auto handle = mq.get_handle();

    for (int round = 0; round < 3; ++round) {
        for (unsigned i = 1; i <= 100; ++i) {
            handle.push(i);
        }
        unsigned popped = 0;
        while (handle.try_pop()) {
            ++popped;
        }
        REQUIRE(popped == 100);
    }
}

// A mode that keeps more pqs than a single handle can be assigned still has to
// work when the queue is wide enough for its candidates to differ, which is what
// exercises the paths that replace or rotate a single candidate.
TEMPLATE_TEST_CASE("mode drains a queue wider than its selection", "[modes][conservation]", MODE_LIST) {
    static constexpr unsigned num_elements = 2000;

    test_mq<TestType> mq(pqs_for(8));
    auto handle = mq.get_handle();

    for (unsigned i = 1; i <= num_elements; ++i) {
        handle.push(i);
    }
    unsigned popped = 0;
    while (handle.try_pop()) {
        ++popped;
    }
    REQUIRE(popped == num_elements);
}

// Stickiness is a per-handle counter; a value of n means the selected pqs are
// used for n *additional* operations, so 0 must reselect on every operation
// rather than sticking forever.
TEMPLATE_TEST_CASE("mode handles every stickiness including zero", "[modes][stickiness]", STICKY_MODE_LIST) {
    int const stickiness = GENERATE(0, 1, 2, 16);
    CAPTURE(stickiness);

    using mq_type = test_mq<TestType>;
    mq_type mq(pqs_for(4), typename mq_type::config_type{1, stickiness});
    auto handle = mq.get_handle();

    for (unsigned i = 1; i <= 1000; ++i) {
        handle.push(i);
    }
    unsigned popped = 0;
    while (handle.try_pop()) {
        ++popped;
    }
    REQUIRE(popped == 1000);
}

// The split modes take a stickiness for popping and one for pushing, and the two
// must be independent, including when one of them reselects on every operation.
TEMPLATE_TEST_CASE("split mode handles every pair of stickinesses", "[modes][stickiness]", SPLIT_STICKY_MODE_LIST) {
    int const pop_stickiness = GENERATE(0, 1, 16);
    int const push_stickiness = GENERATE(0, 1, 64);
    CAPTURE(pop_stickiness, push_stickiness);

    using mq_type = test_mq<TestType>;
    mq_type mq(pqs_for(4), typename mq_type::config_type{1, pop_stickiness, push_stickiness});
    auto handle = mq.get_handle();

    for (unsigned i = 1; i <= 1000; ++i) {
        handle.push(i);
    }
    unsigned popped = 0;
    while (handle.try_pop()) {
        ++popped;
    }
    REQUIRE(popped == 1000);
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
            // A relaxed pop may report empty while other threads still hold
            // elements, so drain against the global counter rather than trusting
            // a single empty result.
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

// Pushes that only land in a push buffer are not in any heap, so a handle that
// never pushes has to be able to find them through the published top keys alone.
TEST_CASE("push buffer hands elements to a handle that never pushed", "[modes][push_buffer]") {
    static constexpr unsigned num_elements = 1000;

    test_mq<ModeRandomPushBuffer> mq(pqs_for(2));
    auto producer = mq.get_handle();
    auto consumer = mq.get_handle();

    for (unsigned i = 1; i <= num_elements; ++i) {
        producer.push(i);
    }

    unsigned popped = 0;
    while (consumer.try_pop()) {
        ++popped;
    }
    REQUIRE(popped == num_elements);
}

TEST_CASE("multiqueue reports its configuration", "[modes][basic]") {
    using mq_type = test_mq<multiqueue::mode::Random<num_pop_candidates>>;
    mq_type mq(pqs_for(4));
    REQUIRE(mq.num_pqs() == pqs_for(4));
    REQUIRE(mq.get_allocator() == std::allocator<typename mq_type::priority_queue_type>{});
    REQUIRE(mq_type::sentinel() == std::numeric_limits<unsigned>::lowest());
}
