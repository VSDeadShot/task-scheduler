#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <latch>
#include <mutex>
#include <numeric>
#include <set>
#include <utility>
#include <thread>
#include <type_traits>
#include <vector>

#include <time.h>  // clock_gettime, CLOCK_PROCESS_CPUTIME_ID (POSIX)

namespace {

// A pool owns running threads that refer back to it, so it must never be
// copied or moved. Checked at compile time: a violation fails the build.
static_assert(!std::is_copy_constructible_v<tsched::ThreadPool>,
              "ThreadPool must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<tsched::ThreadPool>,
              "ThreadPool must not be copy-assignable");
static_assert(!std::is_move_constructible_v<tsched::ThreadPool>,
              "ThreadPool must not be move-constructible");
static_assert(!std::is_move_assignable_v<tsched::ThreadPool>,
              "ThreadPool must not be move-assignable");

// Several counts, so a constant return value cannot pass by coincidence.
TEST(ThreadPoolLifecycle, SizeReportsRequestedThreadCount) {
    for (std::size_t requested : {1u, 3u, 8u}) {
        SCOPED_TRACE(testing::Message() << "requested=" << requested);
        tsched::ThreadPool pool{requested};
        EXPECT_EQ(pool.size(), requested);
    }
}

// A pool with no workers could never run anything, so zero is clamped up.
TEST(ThreadPoolLifecycle, ZeroThreadCountClampsToOne) {
    tsched::ThreadPool pool{0};
    EXPECT_EQ(pool.size(), 1u);
}

// Generous: it only has to outlast thread creation, and a failure here fails the
// test rather than hanging the suite.
constexpr auto kStartTimeout = std::chrono::seconds(5);

TEST(ThreadPoolLifecycle, EveryWorkerStarts) {
    constexpr std::size_t kWorkers = 4;

    std::mutex mutex;
    std::condition_variable started_cv;
    std::vector<std::thread::id> started;  // guarded by mutex

    tsched::ThreadPool::Hooks hooks;
    hooks.on_worker_start = [&](std::size_t /*index*/) {
        {
            std::lock_guard lock(mutex);
            started.push_back(std::this_thread::get_id());
        }
        started_cv.notify_all();
    };

    std::vector<std::thread::id> observed;
    {
        tsched::ThreadPool pool{kWorkers, hooks};

        std::unique_lock lock(mutex);
        const bool all_started = started_cv.wait_for(
            lock, kStartTimeout, [&] { return started.size() >= kWorkers; });
        EXPECT_TRUE(all_started)
            << "only " << started.size() << " of " << kWorkers
            << " workers started within " << kStartTimeout.count() << "s";
        observed = started;
    }

    EXPECT_EQ(observed.size(), kWorkers);

    const std::set<std::thread::id> distinct(observed.begin(), observed.end());
    EXPECT_EQ(distinct.size(), observed.size()) << "worker thread ids were not distinct";
    EXPECT_EQ(distinct.count(std::this_thread::get_id()), 0u)
        << "a start hook ran on the test thread instead of a worker";
}

TEST(ThreadPoolLifecycle, EachWorkerGetsUniqueIndex) {
    constexpr std::size_t kWorkers = 4;

    std::mutex mutex;
    std::condition_variable started_cv;
    std::vector<std::pair<std::size_t, std::thread::id>> seen;  // guarded by mutex

    tsched::ThreadPool::Hooks hooks;
    hooks.on_worker_start = [&](std::size_t index) {
        {
            std::lock_guard lock(mutex);
            seen.emplace_back(index, std::this_thread::get_id());
        }
        started_cv.notify_all();
    };

    std::vector<std::pair<std::size_t, std::thread::id>> observed;
    {
        tsched::ThreadPool pool{kWorkers, hooks};

        std::unique_lock lock(mutex);
        const bool all_started = started_cv.wait_for(
            lock, kStartTimeout, [&] { return seen.size() >= kWorkers; });
        EXPECT_TRUE(all_started)
            << "only " << seen.size() << " of " << kWorkers << " workers started";
        observed = seen;
    }

    ASSERT_EQ(observed.size(), kWorkers);

    std::vector<std::size_t> indices;
    std::set<std::thread::id> threads;
    for (const auto& [index, thread_id] : observed) {
        indices.push_back(index);
        threads.insert(thread_id);
    }
    std::sort(indices.begin(), indices.end());

    std::vector<std::size_t> expected(kWorkers);
    std::iota(expected.begin(), expected.end(), std::size_t{0});
    EXPECT_EQ(indices, expected) << "indices should be exactly 0..N-1, each once";
    EXPECT_EQ(threads.size(), kWorkers) << "indices did not arrive on distinct threads";
}

TEST(ThreadPoolLifecycle, StopJoinsWorkersSynchronously) {
    constexpr std::size_t kWorkers = 4;

    std::mutex mutex;
    std::condition_variable started_cv;
    std::vector<std::thread::id> started;  // guarded by mutex
    std::vector<std::thread::id> exited;   // guarded by mutex

    tsched::ThreadPool::Hooks hooks;
    hooks.on_worker_start = [&](std::size_t /*index*/) {
        {
            std::lock_guard lock(mutex);
            started.push_back(std::this_thread::get_id());
        }
        started_cv.notify_all();
    };
    // No notify: nothing waits on exits. stop() itself must be what guarantees
    // these have all run by the time it returns.
    hooks.on_worker_exit = [&](std::size_t /*index*/) {
        std::lock_guard lock(mutex);
        exited.push_back(std::this_thread::get_id());
    };

    std::set<std::thread::id> start_ids;
    std::set<std::thread::id> exit_ids;
    {
        tsched::ThreadPool pool{kWorkers, hooks};

        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(started_cv.wait_for(lock, kStartTimeout,
                                            [&] { return started.size() >= kWorkers; }))
                << "only " << started.size() << " of " << kWorkers << " workers started";
        }  // the lock must be released here: the exit hooks take this same mutex.

        pool.stop();

        // Checked immediately, with no waiting of any kind: an asynchronous
        // stop() must fail here rather than be papered over by a timeout.
        {
            std::lock_guard lock(mutex);
            EXPECT_EQ(exited.size(), kWorkers)
                << "stop() returned before every worker had exited";
            start_ids.insert(started.begin(), started.end());
            exit_ids.insert(exited.begin(), exited.end());
        }
        EXPECT_EQ(exit_ids, start_ids) << "exit hooks did not run on the worker threads";

        pool.stop();  // idempotent: must not hang, crash, or re-run hooks
        {
            std::lock_guard lock(mutex);
            EXPECT_EQ(exited.size(), kWorkers) << "a second stop() re-ran the exit hooks";
        }
    }  // destructor after an explicit stop(): must not double-join

    std::lock_guard lock(mutex);
    EXPECT_EQ(exited.size(), kWorkers) << "the destructor re-ran the exit hooks after stop()";
}

TEST(ThreadPoolLifecycle, StoppedReflectsLifecycle) {
    tsched::ThreadPool pool{4};

    EXPECT_FALSE(pool.stopped()) << "a freshly built pool is running, not stopped";

    pool.stop();
    // stop() is synchronous, so this needs no waiting of any kind.
    EXPECT_TRUE(pool.stopped()) << "stopped() should be true as soon as stop() returns";

    pool.stop();
    EXPECT_TRUE(pool.stopped()) << "a second stop() must not un-stop the pool";
}

TEST(ThreadPoolLifecycle, DestructorStopsAndJoinsWithoutExplicitStop) {
    constexpr std::size_t kWorkers = 4;

    std::mutex mutex;
    std::condition_variable started_cv;
    std::vector<std::thread::id> started;  // guarded by mutex
    std::vector<std::thread::id> exited;   // guarded by mutex

    tsched::ThreadPool::Hooks hooks;
    hooks.on_worker_start = [&](std::size_t /*index*/) {
        {
            std::lock_guard lock(mutex);
            started.push_back(std::this_thread::get_id());
        }
        started_cv.notify_all();
    };
    hooks.on_worker_exit = [&](std::size_t /*index*/) {
        std::lock_guard lock(mutex);
        exited.push_back(std::this_thread::get_id());
    };

    {
        tsched::ThreadPool pool{kWorkers, hooks};

        std::unique_lock lock(mutex);
        ASSERT_TRUE(started_cv.wait_for(lock, kStartTimeout,
                                        [&] { return started.size() >= kWorkers; }))
            << "only " << started.size() << " of " << kWorkers << " workers started";
        // Deliberately no stop() call: the destructor must do the whole job.
    }

    // Checked on the very next line, with no waiting: the destructor is
    // required to have joined every worker before it returned.
    std::lock_guard lock(mutex);
    EXPECT_EQ(exited.size(), kWorkers)
        << "the destructor returned before every worker had exited";
    const std::set<std::thread::id> start_ids(started.begin(), started.end());
    const std::set<std::thread::id> exit_ids(exited.begin(), exited.end());
    EXPECT_EQ(exit_ids, start_ids) << "exit hooks did not run on the worker threads";
}

// Behavior 10: idle workers sleep rather than spin.
//
// Measured as process CPU time (all threads, user + system) over an idle window
// that starts only after every worker has started, so startup cost is excluded.
// A parked worker is blocked in the kernel and costs ~nothing; measured locally
// at 0.1 ms (g++) and 0.25 ms (clang++/TSan) per 250 ms window. Four spinning
// workers cost ~1000 ms, and even one spinning worker ~250 ms, so the 50 ms
// threshold has wide margins on both sides. Other processes cannot bill CPU time
// to this one, so machine load cannot push a sleeping pool over the threshold.
//
// Limitation: this catches spinning, not slow polling. A worker that loops on
// sleep_for(1ms) costs only a millisecond or two over the window and would pass.
// Telling "woken by notification" apart from "polling" would need voluntary
// context-switch counts (getrusage ru_nvcsw), which is out of scope here.
//
// Portability: POSIX-only (CLOCK_PROCESS_CPUTIME_ID). A Windows port needs
// GetProcessTimes instead. std::clock() is not a substitute: on MSVC it returns
// wall-clock time, so a sleeping pool would appear to burn the whole window.
double process_cpu_ms() {
    ::timespec ts{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        ADD_FAILURE() << "clock_gettime(CLOCK_PROCESS_CPUTIME_ID) failed";
    }
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
}

TEST(ThreadPoolLifecycle, IdleWorkersSleepRatherThanSpin) {
    constexpr std::size_t kWorkers = 4;
    constexpr auto kIdleWindow = std::chrono::milliseconds(250);
    constexpr double kMaxIdleCpuMs = 50.0;

    std::mutex mutex;
    std::condition_variable started_cv;
    std::size_t started = 0;  // guarded by mutex

    tsched::ThreadPool::Hooks hooks;
    hooks.on_worker_start = [&](std::size_t /*index*/) {
        {
            std::lock_guard lock(mutex);
            ++started;
        }
        started_cv.notify_all();
    };

    tsched::ThreadPool pool{kWorkers, hooks};
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(started_cv.wait_for(lock, kStartTimeout,
                                        [&] { return started >= kWorkers; }))
            << "only " << started << " of " << kWorkers << " workers started";
    }

    const double cpu_before_ms = process_cpu_ms();
    std::this_thread::sleep_for(kIdleWindow);  // the test thread itself costs ~0 here
    const double idle_cpu_ms = process_cpu_ms() - cpu_before_ms;

    EXPECT_LT(idle_cpu_ms, kMaxIdleCpuMs)
        << kWorkers << " idle workers burned " << idle_cpu_ms << " ms of CPU over a "
        << kIdleWindow.count() << " ms window: they are spinning instead of sleeping";
}

// Behavior 12: several threads calling stop() at once must all get the full
// guarantee, not just the first. Every caller, the moment its own stop()
// returns, must see every worker exited and stopped() == true.
TEST(ThreadPoolLifecycle, ConcurrentStopCallersAllObserveFullJoin) {
    constexpr std::size_t kWorkers = 4;
    constexpr std::size_t kStoppers = 4;
    // Each exit hook sleeps (outside the lock) so the first stop() takes at
    // least this long. That makes a broken stop(), one that lets later callers
    // return before the joins finish, fail every time rather than only when the
    // scheduler happens to overlap the callers. Correct code is unaffected: a
    // caller blocked in std::call_once returns only after the joins complete.
    constexpr auto kExitHookDelay = std::chrono::milliseconds(50);

    std::mutex mutex;
    std::condition_variable started_cv;
    std::size_t started = 0;  // guarded by mutex
    std::size_t exited = 0;   // guarded by mutex

    tsched::ThreadPool::Hooks hooks;
    hooks.on_worker_start = [&](std::size_t /*index*/) {
        {
            std::lock_guard lock(mutex);
            ++started;
        }
        started_cv.notify_all();
    };
    hooks.on_worker_exit = [&](std::size_t /*index*/) {
        std::this_thread::sleep_for(kExitHookDelay);
        std::lock_guard lock(mutex);
        ++exited;
    };

    struct Observation {
        std::size_t exits_seen = 0;
        bool stopped_seen = false;
    };
    std::array<Observation, kStoppers> observed{};  // slot i written only by stopper i

    {
        tsched::ThreadPool pool{kWorkers, hooks};
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(started_cv.wait_for(lock, kStartTimeout,
                                            [&] { return started >= kWorkers; }))
                << "only " << started << " of " << kWorkers << " workers started";
        }

        // Released together: nobody calls stop() until all have arrived.
        std::latch ready{static_cast<std::ptrdiff_t>(kStoppers)};
        {
            std::vector<std::jthread> stoppers;
            stoppers.reserve(kStoppers);
            for (std::size_t i = 0; i < kStoppers; ++i) {
                stoppers.emplace_back([&, i] {
                    ready.arrive_and_wait();
                    pool.stop();
                    // Recorded the moment this caller's own stop() returns.
                    const bool stopped_now = pool.stopped();
                    std::lock_guard lock(mutex);
                    observed[i] = Observation{exited, stopped_now};
                });
            }
        }  // stoppers joined here, before the pool is destroyed
    }

    for (std::size_t i = 0; i < kStoppers; ++i) {
        EXPECT_EQ(observed[i].exits_seen, kWorkers)
            << "stop() caller " << i << " returned before every worker had exited";
        EXPECT_TRUE(observed[i].stopped_seen)
            << "stop() caller " << i << " returned while stopped() was still false";
    }
    std::lock_guard lock(mutex);
    EXPECT_EQ(exited, kWorkers) << "exit hooks ran more than once per worker";
}

}  // namespace
