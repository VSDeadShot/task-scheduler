#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Whether the future holds a result within `wait`, by name, so a failed check
// reads "timeout" rather than the raw bytes GoogleTest prints for
// std::future_status. Waiting with a bound means a wiring bug fails the test
// instead of hanging it.
template <typename T>
std::string status_within(const std::future<T>& result, std::chrono::milliseconds wait) {
    switch (result.wait_for(wait)) {
        case std::future_status::ready:
            return "ready";
        case std::future_status::timeout:
            return "timeout";
        case std::future_status::deferred:
            return "deferred";
    }
    return "unknown";
}

constexpr auto kResultTimeout = std::chrono::milliseconds(5000);

// Every submitted task runs on one of the pool's own workers, never inline on
// the thread that submitted it, and its result comes back through its future.
TEST(ThreadPoolSubmit, SubmittedTaskRunsOnAWorkerAndReturnsItsResult) {
    constexpr std::size_t kWorkers = 2;
    constexpr int kTasks = 100;

    // Each worker records its id before it can run anything, on the same thread
    // that later runs tasks, so the future's get() makes the record visible here.
    std::mutex mutex;
    std::set<std::thread::id> worker_ids;  // guarded by mutex
    tsched::ThreadPool::Hooks hooks;
    hooks.on_worker_start = [&](std::size_t /*index*/) {
        std::lock_guard lock{mutex};
        worker_ids.insert(std::this_thread::get_id());
    };

    tsched::ThreadPool pool{kWorkers, hooks};
    std::vector<std::future<std::pair<int, std::thread::id>>> results;
    for (int i = 0; i < kTasks; ++i) {
        results.push_back(pool.submit([i] { return std::pair{i * i, std::this_thread::get_id()}; }));
    }

    const std::thread::id test_thread = std::this_thread::get_id();
    int wrong_values = 0;
    int ran_on_submitter = 0;
    int ran_off_pool = 0;
    for (int i = 0; i < kTasks; ++i) {
        ASSERT_EQ(status_within(results[i], kResultTimeout), "ready") << "task " << i;
        const auto [value, ran_on] = results[i].get();
        if (value != i * i) {
            ++wrong_values;
        }
        std::lock_guard lock{mutex};
        if (ran_on == test_thread) {
            ++ran_on_submitter;
        } else if (!worker_ids.contains(ran_on)) {
            ++ran_off_pool;
        }
    }
    EXPECT_EQ(wrong_values, 0);
    EXPECT_EQ(ran_on_submitter, 0) << "tasks ran inline on the submitting thread";
    EXPECT_EQ(ran_off_pool, 0) << "tasks ran on a thread that is not one of the pool's workers";
}

}  // namespace
