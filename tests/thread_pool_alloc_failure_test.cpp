// Tests that need a failing allocation. Linked with fail_alloc.cpp, which
// replaces the global operator new for this executable only.
#include "fail_alloc.hpp"
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

#include <time.h>  // clock_gettime, CLOCK_PROCESS_CPUTIME_ID (POSIX)

namespace {

// Copied from thread_pool_lifecycle_test.cpp (behavior 10), which explains the
// method, its margins and its POSIX-only limitation: process CPU time, all
// threads, in milliseconds.
double process_cpu_ms() {
    ::timespec ts{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        ADD_FAILURE() << "clock_gettime(CLOCK_PROCESS_CPUTIME_ID) failed";
    }
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
}

// Counts a latch down when it goes out of scope, so a blocked task is always
// released, even when an assertion ends the test early. release() lets a test
// release it earlier; it counts down only once.
struct ReleaseOnExit {
    std::latch& latch;
    bool released = false;

    void release() {
        if (!released) {
            released = true;
            latch.count_down();
        }
    }
    ~ReleaseOnExit() { release(); }
};

// The allocation a deque push makes when its current block is full. libstdc++'s
// std::deque allocates blocks of _GLIBCXX_DEQUE_BUF_SIZE = 512 bytes for any
// element smaller than that, and detail::Task is 8 bytes (one pointer), so a
// block holds 64 tasks. Nothing else submit() allocates for an empty lambda is
// this size (the task's shared state, its Task holder and its result slot are
// 64, 16 and 24 bytes). If a different standard library or a different Task
// layout changes any of that, no submit fails, and the vacuity check below
// says so instead of the test passing on nothing.
constexpr std::size_t kDequeBlockBytes = 512;

// Run with stealing on and off, because each mode reads a different count: with
// stealing on, a worker decides whether to sleep or leave from total_queued_
// alone, and queued_[i] only matters with stealing off. A rollback that forgets
// one of the two counts passes in one mode and fails in the other.
class ThreadPoolAllocFailure : public ::testing::TestWithParam<bool> {};

INSTANTIATE_TEST_SUITE_P(, ThreadPoolAllocFailure, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "StealingOn" : "StealingOff";
                         });

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

    // stop() on another thread, with a bound, so a stop() that never returns
    // fails the test instead of hanging it. The stopper holds nothing on this
    // stack: if it is left behind below and stop() returns late after all, it
    // only touches the leaked pool and a promise it shares ownership of.
    auto stop_returned = std::make_shared<std::promise<void>>();
    std::future<void> stopped = stop_returned->get_future();
    std::thread stopper{[target = pool.get(), stop_returned] {
        target->stop();
        stop_returned->set_value();
    }};
    const bool returned = stopped.wait_for(kStopBound) == std::future_status::ready;
    EXPECT_EQ(returned ? "returned" : "timeout", std::string{"returned"})
        << "stop() never returned: it is waiting for the failed submit's task";
    if (returned) {
        stopper.join();
    } else {
        // Failure path only. The stopper is stuck joining a worker that will
        // never leave, so let both go: the process can still exit, and ctest
        // runs each test in its own process, so nothing else shares it.
        stopper.detach();
        static_cast<void>(pool.release());
    }
}

}  // namespace
