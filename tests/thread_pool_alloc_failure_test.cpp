// Tests that need a failing allocation. Linked with fail_alloc.cpp, which
// replaces the global operator new for this executable only.
#include "fail_alloc.hpp"
#include "test_support.hpp"
#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <latch>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using tsched_test::process_cpu_ms;
using tsched_test::ReleaseOnExit;
using tsched_test::stealing_param_name;
using tsched_test::stop_within;

// The allocation a deque push makes when its current block is full. libstdc++'s
// std::deque allocates blocks of _GLIBCXX_DEQUE_BUF_SIZE = 512 bytes for any
// element smaller than that, and detail::Task is 8 bytes (one pointer), so a
// block holds 64 tasks. Nothing else submit() allocates for an empty lambda is
// this size (the task's shared state, its result slot and its Task holder are
// 64, 16 and 24 bytes). If a different standard library or a different Task
// layout changes any of that, no submit fails, and the vacuity check below
// says so instead of the test passing on nothing.
constexpr std::size_t kDequeBlockBytes = 512;

// Run with stealing on and off, because each mode reads a different count: with
// stealing on, a worker decides whether to sleep or leave from total_queued_
// alone, and queued_[i] only matters with stealing off. A rollback that forgets
// one of the two counts, or a reservation left raised on only one of them,
// passes in one mode and fails in the other.
class ThreadPoolAllocFailure : public ::testing::TestWithParam<bool> {};

INSTANTIATE_TEST_SUITE_P(, ThreadPoolAllocFailure, ::testing::Bool(), stealing_param_name);

// A submit() whose push fails after its slot was reserved leaves the pool as if
// it had never been called: the reservation is released, so the idle worker
// goes back to sleep instead of rescanning for a task that will never arrive,
// and stop() returns instead of waiting for that task forever.
TEST_P(ThreadPoolAllocFailure, FailedPushReleasesItsReservation) {
    constexpr int kMaxSubmits = 1000;
    constexpr auto kStartBound = std::chrono::seconds(10);
    constexpr auto kRunBound = std::chrono::seconds(10);
    constexpr auto kStopBound = std::chrono::seconds(10);
    constexpr auto kIdleWindow = std::chrono::milliseconds(250);
    constexpr double kMaxIdleCpuMs = 50.0;

    std::latch release_blocker{1};
    std::promise<void> blocker_started;
    std::future<void> started = blocker_started.get_future();
    std::atomic<int> ran{0};
    // On the heap so the failure path below can leak it: destroying a pool
    // whose stop() never returns would hang the test instead of failing it.
    auto pool = std::make_unique<tsched::ThreadPool>(1, tsched::ThreadPool::Hooks{},
                                                     tsched::PoolOptions{.stealing = GetParam()});
    ReleaseOnExit release{release_blocker};

    // The worker stays busy, so every task submitted next stays in its deque,
    // and the deque has to grow by a block once 64 items are in it.
    std::future<void> blocker = pool->submit([&] {
        blocker_started.set_value();
        release_blocker.wait();
    });
    ASSERT_EQ(started.wait_for(kStartBound), std::future_status::ready) << "the blocker never started";

    // Reserved up front: growing this vector while armed could be the
    // allocation that fails, outside the pool entirely.
    std::vector<std::future<void>> accepted;
    accepted.reserve(kMaxSubmits);
    int failed_submits = 0;
    bool failure_was_the_seam = false;
    for (int i = 0; i < kMaxSubmits && failed_submits == 0; ++i) {
        std::future<void> result;
        {
            tsched_test::FailNextAllocationOfSize fail{kDequeBlockBytes};
            try {
                result = pool->submit([&ran] { ran.fetch_add(1); });
            } catch (const std::bad_alloc&) {
                ++failed_submits;
                failure_was_the_seam = fail.fired();
            }
        }
        if (result.valid()) {
            accepted.push_back(std::move(result));
        }
    }
    // Vacuity check: without exactly one failed push there is nothing to test.
    ASSERT_EQ(failed_submits, 1) << "no submit() failed: the deque's block size or Task's size has changed";
    ASSERT_TRUE(failure_was_the_seam) << "the bad_alloc did not come from the test's allocation seam";

    release.release();
    const auto run_deadline = std::chrono::steady_clock::now() + kRunBound;
    int not_ready = 0;
    for (std::future<void>& result : accepted) {
        if (result.wait_until(run_deadline) != std::future_status::ready) {
            ++not_ready;
        }
    }
    EXPECT_EQ(not_ready, 0) << "tasks accepted before the failed submit never ran";
    EXPECT_EQ(ran.load(), static_cast<int>(accepted.size())) << "the failed submit's task ran, or an accepted one didn't";

    // Every accepted task has finished, so the worker should now be asleep.
    const double cpu_before_ms = process_cpu_ms();
    std::this_thread::sleep_for(kIdleWindow);  // the test thread itself costs ~0 here
    const double idle_cpu_ms = process_cpu_ms() - cpu_before_ms;
    EXPECT_LT(idle_cpu_ms, kMaxIdleCpuMs)
        << "the idle worker burned " << idle_cpu_ms << " ms of CPU over a " << kIdleWindow.count()
        << " ms window: it is rescanning for the failed submit's task instead of sleeping";

    // stop() with a bound, so a stop() that never returns fails the test
    // instead of hanging it; on that failure the pool is leaked (see
    // stop_within()).
    const bool returned = stop_within(pool, kStopBound);
    EXPECT_EQ(returned ? "returned" : "timeout", std::string{"returned"})
        << "stop() never returned: it is waiting for the failed submit's task";
}

// A submit() that fails while building its task leaves the pool untouched:
// building happens before enqueue() reserves anything, so there is nothing to
// roll back. Building allocates the packaged_task's shared state and result
// slot, copies or moves the callable into it, then allocates the Task holder.
// Rather than depend on how many allocations that is, or on their sizes, the
// test fails the first one, then the second, and so on, until a submit() gets
// through without reaching the allocation armed to fail; by then every
// allocation submit() makes has failed once. A step that left a slot reserved
// would show up as in FailedPushReleasesItsReservation: a worker rescanning
// instead of sleeping, and a stop() that never returns.
TEST_P(ThreadPoolAllocFailure, FailedTaskConstructionReservesNothing) {
    constexpr std::size_t kMaxSteps = 64;
    constexpr auto kRunBound = std::chrono::seconds(10);
    constexpr auto kStopBound = std::chrono::seconds(10);
    constexpr auto kIdleWindow = std::chrono::milliseconds(250);
    constexpr double kMaxIdleCpuMs = 50.0;

    std::atomic<int> ran{0};
    // On the heap so the failure path below can leak it, as above.
    auto pool = std::make_unique<tsched::ThreadPool>(1, tsched::ThreadPool::Hooks{},
                                                     tsched::PoolOptions{.stealing = GetParam()});

    int failed_steps = 0;
    int failures_not_from_seam = 0;
    bool got_through = false;
    bool got_through_after_seam_fired = false;
    std::future<void> accepted;
    for (std::size_t n = 1; n <= kMaxSteps && !got_through; ++n) {
        tsched_test::FailNthAllocation fail{n};
        try {
            accepted = pool->submit([&ran] { ran.fetch_add(1); });
            got_through = true;
            got_through_after_seam_fired = fail.fired();
        } catch (const std::bad_alloc&) {
            ++failed_steps;
            if (!fail.fired()) {
                ++failures_not_from_seam;
            }
        }
    }
    RecordProperty("failed_steps", failed_steps);
    // Guards against a sweep that tests nothing: one that never ends, one in
    // which no allocation ever failed, and one whose failures weren't ours.
    ASSERT_TRUE(got_through) << "submit() failed at each of the first " << kMaxSteps
                             << " allocations and never got through: the seam keeps firing";
    ASSERT_GE(failed_steps, 1) << "no allocation inside submit() failed: the seam never armed, so nothing was tested";
    ASSERT_EQ(failures_not_from_seam, 0) << "a bad_alloc did not come from the test's allocation seam";
    ASSERT_FALSE(got_through_after_seam_fired) << "submit() swallowed the allocation failure instead of throwing it";

    // The submit() that got through came right after a failed one, so the pool
    // is still usable; and none of the failed steps queued a task that ran.
    EXPECT_EQ(accepted.wait_for(kRunBound) == std::future_status::ready ? "ready" : "timeout", std::string{"ready"})
        << "the task submitted after the failed steps never ran";
    EXPECT_EQ(ran.load(), 1) << "a failed submit's task ran, or the accepted one didn't";

    // The worker has nothing left to do, so it should be asleep.
    const double cpu_before_ms = process_cpu_ms();
    std::this_thread::sleep_for(kIdleWindow);
    const double idle_cpu_ms = process_cpu_ms() - cpu_before_ms;
    EXPECT_LT(idle_cpu_ms, kMaxIdleCpuMs)
        << "the idle worker burned " << idle_cpu_ms << " ms of CPU over a " << kIdleWindow.count()
        << " ms window: it is rescanning for a task a failed submit left reserved";

    // stop() with a bound; see FailedPushReleasesItsReservation.
    const bool returned = stop_within(pool, kStopBound);
    EXPECT_EQ(returned ? "returned" : "timeout", std::string{"returned"})
        << "stop() never returned: it is waiting for a task a failed submit left reserved";
}

}  // namespace
