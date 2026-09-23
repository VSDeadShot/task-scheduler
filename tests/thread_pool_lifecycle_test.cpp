#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <numeric>
#include <set>
#include <utility>
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

}  // namespace
