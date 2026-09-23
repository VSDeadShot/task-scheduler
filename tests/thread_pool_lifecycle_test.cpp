#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

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

}  // namespace
