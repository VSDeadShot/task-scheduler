#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
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

// Long enough for any worker to pick up its task, so missing it means the tasks
// were queued behind one another rather than running at once.
constexpr auto kRendezvousDeadline = std::chrono::seconds(2);
// Bound on waiting for a result that is expected within the deadline above.
constexpr auto kResultBound = std::chrono::milliseconds(10'000);

// Tells a group of tasks whether they were all running at the same time. Each
// one arrives and then waits until the whole group has arrived or the deadline
// passes. Tasks queued behind one another on one worker can never all meet:
// the first gives up at the deadline, and the rest arrive after it.
class Rendezvous {
public:
    Rendezvous(int group_size, std::chrono::steady_clock::time_point deadline)
        : group_size_(group_size), deadline_(deadline) {}

    bool arrive_and_wait() {
        std::unique_lock lock{mutex_};
        // Only a group completed before the deadline counts; otherwise the last
        // of a serialized group would see everyone "arrived" and report success.
        if (++arrived_ == group_size_ && std::chrono::steady_clock::now() < deadline_) {
            all_met_ = true;
            met_.notify_all();
        }
        return met_.wait_until(lock, deadline_, [this] { return all_met_; });
    }

private:
    const int group_size_;
    const std::chrono::steady_clock::time_point deadline_;
    std::mutex mutex_;
    std::condition_variable met_;
    int arrived_ = 0;       // guarded by mutex_
    bool all_met_ = false;  // guarded by mutex_
};

// Submissions from outside the pool are spread across its workers, so as many
// tasks as there are workers can all run at once rather than queueing behind
// one another on a single worker.
TEST(ThreadPoolSubmit, ExternalSubmissionsCanAllRunAtOnce) {
    constexpr int kWorkers = 4;
    // Declared before the pool so it outlives every task that refers to it.
    Rendezvous rendezvous{kWorkers, std::chrono::steady_clock::now() + kRendezvousDeadline};
    tsched::ThreadPool pool{kWorkers};

    std::vector<std::future<bool>> met;
    for (int i = 0; i < kWorkers; ++i) {
        met.push_back(pool.submit([&rendezvous] { return rendezvous.arrive_and_wait(); }));
    }
    int ran_together = 0;
    for (std::future<bool>& result : met) {
        ASSERT_EQ(status_within(result, kResultBound), "ready");
        if (result.get()) {
            ++ran_together;
        }
    }
    EXPECT_EQ(ran_together, kWorkers) << "tasks were queued behind one another instead of running at once";
}

// Counts a latch down when it goes out of scope, so a blocked task is always
// released, even when an assertion ends the test early. Otherwise the pool's
// destructor would wait forever to join the blocked worker. release() lets a
// test release it earlier; it counts down only once, since counting a latch
// below zero is undefined.
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

// A task submitted from one of the pool's own workers goes onto that worker's
// own deque, so it runs on that worker next rather than queueing behind some
// other worker. Here the other worker is blocked: a child that went round-robin
// like an outside submission would land behind it (submissions 1 and 3 share a
// deque on a 2-worker pool, whatever the starting slot) and not run until the
// blocker is released.
//
// Runs with stealing off. With it on, this test could not detect misrouting at
// all: a child sent to the blocked worker's deque would simply be stolen by the
// parent's idle worker and run on the parent's thread, so every assertion below
// would still pass.
TEST(ThreadPoolSubmit, WorkersOwnSubmissionsStayOnItsDeque) {
    constexpr auto kChildBound = std::chrono::seconds(2);
    // Declared before the pool, and the guard after it: the guard releases the
    // blocker before the pool's destructor joins, and the latch outlives both.
    std::latch release_blocker{1};
    tsched::ThreadPool pool{2, {}, {.stealing = false}};
    ReleaseOnExit release{release_blocker};

    std::future<void> blocker = pool.submit([&release_blocker] { release_blocker.wait(); });
    auto parent = pool.submit([&pool] {
        std::future<std::thread::id> child = pool.submit([] { return std::this_thread::get_id(); });
        return std::pair{std::this_thread::get_id(), std::move(child)};
    });

    ASSERT_EQ(status_within(parent, kResultBound), "ready");
    auto [parent_thread, child] = parent.get();
    ASSERT_EQ(status_within(child, kChildBound), "ready") << "the child was queued behind the blocked worker";
    EXPECT_EQ(child.get(), parent_thread) << "the child ran on a different worker than its parent";
}

// A worker of some *other* pool is an outsider here: its submissions are spread
// round-robin like any other thread's. Its worker index belongs to its own pool,
// so using it here would pile every submission onto one deque, or, if its pool
// were the larger one, index past the end of this pool's deques.
//
// The sizes are chosen so that bug fails safely: the 1-worker pool's only index
// is 0, which is in range for the 2-worker target, so both tasks would land on
// one deque and run one after the other instead of meeting.
//
// The target runs with stealing off. With it on, its idle worker would steal
// one of the two misrouted tasks, they would still meet, and this test could
// not detect the bug.
TEST(ThreadPoolSubmit, WorkerOfAnotherPoolCountsAsExternal) {
    constexpr int kTasks = 2;
    // Declared first so it outlives both pools; `other` is destroyed before `target`.
    Rendezvous rendezvous{kTasks, std::chrono::steady_clock::now() + kRendezvousDeadline};
    tsched::ThreadPool target{2, {}, {.stealing = false}};
    tsched::ThreadPool other{1};

    auto meet = [&rendezvous] { return rendezvous.arrive_and_wait(); };
    auto submitted = other.submit([&] { return std::pair{target.submit(meet), target.submit(meet)}; });

    ASSERT_EQ(status_within(submitted, kResultBound), "ready");
    auto [first, second] = submitted.get();
    int ran_together = 0;
    for (std::future<bool>* result : {&first, &second}) {
        ASSERT_EQ(status_within(*result, kResultBound), "ready");
        if (result->get()) {
            ++ran_together;
        }
    }
    EXPECT_EQ(ran_together, kTasks) << "another pool's worker had its submissions routed as if it were one of ours";
}

// submit() hands back a future of whatever the task returns: void for a task
// that returns nothing, and a move-only type for a move-only result. A
// move-only callable must be accepted at all, since the pool's own tasks
// (std::packaged_task) can only be moved.
static_assert(std::is_same_v<decltype(std::declval<tsched::ThreadPool&>().submit([] {})), std::future<void>>,
              "a task returning nothing must give std::future<void>");
static_assert(std::is_same_v<decltype(std::declval<tsched::ThreadPool&>().submit(
                                 [owned = std::unique_ptr<int>{}]() mutable { return std::move(owned); })),
                             std::future<std::unique_ptr<int>>>,
              "a move-only task returning a move-only value must be accepted, giving a future of that value");

// A void task's effects are visible once get() returns, and a move-only result
// comes back as the very object the task held: not a copy, not a default value.
TEST(ThreadPoolSubmit, VoidAndMoveOnlyTasksRoundTrip) {
    // A plain bool, not an atomic: get() returning must be what makes the
    // task's write visible here, and ThreadSanitizer checks that it is.
    // Declared before the pool so it outlives the task that writes it.
    bool ran = false;
    auto owned = std::make_unique<int>(7);
    int* const address = owned.get();
    tsched::ThreadPool pool{2};

    std::future<void> done = pool.submit([&ran] { ran = true; });
    std::future<std::unique_ptr<int>> back = pool.submit([held = std::move(owned)]() mutable { return std::move(held); });

    ASSERT_EQ(status_within(done, kResultBound), "ready");
    done.get();
    EXPECT_TRUE(ran) << "get() returned but the void task's write is not visible";

    ASSERT_EQ(status_within(back, kResultBound), "ready");
    std::unique_ptr<int> returned = back.get();
    EXPECT_EQ(returned.get(), address) << "the move-only result is not the object the task held";
}

// A task that throws hands its exception to the caller through its future, with
// its type and message intact, rather than losing it or replacing it with a value.
TEST(ThreadPoolSubmit, TaskExceptionReachesItsFuture) {
    tsched::ThreadPool pool{2};
    std::future<int> result = pool.submit([]() -> int { throw std::runtime_error("boom"); });

    ASSERT_EQ(status_within(result, kResultBound), "ready");
    try {
        result.get();
        FAIL() << "get() returned instead of rethrowing the task's exception";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string{error.what()}, "boom");
    }
}

// A throwing task does not take its worker down: on a 1-worker pool, the next
// task still runs, on the same worker, which has neither exited nor restarted.
TEST(ThreadPoolSubmit, WorkerSurvivesAThrowingTask) {
    constexpr auto kNextTaskBound = std::chrono::seconds(5);
    std::atomic<int> starts{0};
    std::atomic<int> exits{0};
    tsched::ThreadPool::Hooks hooks;
    hooks.on_worker_start = [&](std::size_t /*index*/) { starts.fetch_add(1); };
    hooks.on_worker_exit = [&](std::size_t /*index*/) { exits.fetch_add(1); };
    tsched::ThreadPool pool{1, hooks};

    std::future<void> thrower = pool.submit([] { throw std::runtime_error("boom"); });
    std::future<int> next = pool.submit([] { return 42; });

    // Not an ASSERT, so the exit count below is checked even when this fails;
    // get() is only called on a ready future, so the test can never block.
    const std::string next_status = status_within(next, kNextTaskBound);
    EXPECT_EQ(next_status, "ready") << "the task after a throwing one never ran";
    if (next_status == "ready") {
        EXPECT_EQ(next.get(), 42);
    }
    EXPECT_EQ(starts.load(), 1);
    EXPECT_EQ(exits.load(), 0) << "the worker exited after a task threw";
}

// A worker runs its own queue in FIFO order, so on a 1-worker pool, where every
// task lands in that one queue, tasks run in exactly the order submitted.
TEST(ThreadPoolSubmit, SingleWorkerRunsSubmissionsInOrder) {
    constexpr int kTasks = 100;
    // Written only by the pool's single worker and read only after every
    // future is ready, which is what makes the writes visible here.
    std::vector<int> ran_in_order;
    tsched::ThreadPool pool{1};

    std::vector<std::future<void>> done;
    for (int i = 0; i < kTasks; ++i) {
        done.push_back(pool.submit([&ran_in_order, i] { ran_in_order.push_back(i); }));
    }
    for (std::future<void>& result : done) {
        ASSERT_EQ(status_within(result, kResultBound), "ready");
        result.get();
    }

    std::vector<int> submitted_order(kTasks);
    std::iota(submitted_order.begin(), submitted_order.end(), 0);
    EXPECT_EQ(ran_in_order, submitted_order);
}

// A worker with nothing of its own takes work queued behind a busy worker,
// rather than sitting idle while that work waits.
TEST(ThreadPoolSubmit, IdleWorkerStealsFromABlockedWorker) {
    constexpr int kTasks = 10;
    constexpr auto kStartBound = std::chrono::seconds(10);
    // Plenty for an idle worker to run ten empty tasks; missing it means the
    // tasks behind the blocker were left waiting.
    constexpr auto kStealDeadline = std::chrono::seconds(2);

    // Declared before the pool, and the guard after it: the guard releases the
    // blocker before the pool's destructor joins, and the latch and promise
    // outlive both.
    std::latch release_blocker{1};
    std::promise<void> blocker_started;
    std::future<void> started = blocker_started.get_future();
    tsched::ThreadPool pool{2};
    ReleaseOnExit release{release_blocker};

    std::future<void> blocker = pool.submit([&] {
        blocker_started.set_value();
        release_blocker.wait();
    });
    ASSERT_EQ(status_within(started, kStartBound), "ready") << "the blocker never started";

    // Round-robin puts every other task on the blocked worker's deque, whichever
    // worker ended up running the blocker. Only stealing can get those run now.
    std::vector<std::future<void>> queued;
    for (int i = 0; i < kTasks; ++i) {
        queued.push_back(pool.submit([] {}));
    }
    const auto deadline = std::chrono::steady_clock::now() + kStealDeadline;
    int ready = 0;
    for (std::future<void>& result : queued) {
        if (result.wait_until(deadline) == std::future_status::ready) {
            ++ready;
        }
    }
    EXPECT_EQ(ready, kTasks) << "tasks queued behind the blocked worker were left waiting instead of being stolen";
}

// With stealing turned off, a task waits for the worker it was routed to even
// while another worker sits idle: the tasks behind a blocked worker stay queued
// until it is released, and then they all run.
TEST(ThreadPoolSubmit, StealingCanBeTurnedOff) {
    constexpr int kTasks = 10;
    constexpr auto kStartBound = std::chrono::seconds(10);
    // Long enough for the free worker to finish its own share many times over.
    constexpr auto kWaitWhileBlocked = std::chrono::seconds(1);

    std::latch release_blocker{1};
    std::promise<void> blocker_started;
    std::future<void> started = blocker_started.get_future();
    tsched::ThreadPool pool{2, {}, {.stealing = false}};
    ReleaseOnExit release{release_blocker};

    std::future<void> blocker = pool.submit([&] {
        blocker_started.set_value();
        release_blocker.wait();
    });
    ASSERT_EQ(status_within(started, kStartBound), "ready") << "the blocker never started";

    // Without stealing the blocker runs on the worker it was routed to, and
    // round-robin puts every other task behind it.
    std::vector<std::future<void>> queued;
    for (int i = 0; i < kTasks; ++i) {
        queued.push_back(pool.submit([] {}));
    }
    const auto deadline = std::chrono::steady_clock::now() + kWaitWhileBlocked;
    int ready_while_blocked = 0;
    for (std::future<void>& result : queued) {
        if (result.wait_until(deadline) == std::future_status::ready) {
            ++ready_while_blocked;
        }
    }
    EXPECT_EQ(ready_while_blocked, kTasks / 2) << "tasks behind the blocked worker ran: stealing was not turned off";

    release.release();
    int ready_after_release = 0;
    for (std::future<void>& result : queued) {
        if (status_within(result, kResultBound) == "ready") {
            ++ready_after_release;
        }
    }
    EXPECT_EQ(ready_after_release, kTasks) << "tasks behind the blocker never ran after it was released";
}

// Once stop() has begun, submit() turns work away with operation_canceled
// rather than queueing it, and the rejected task never runs.
TEST(ThreadPoolSubmit, SubmitAfterStopThrows) {
    // Declared before the pool so it outlives anything the pool could still run.
    std::atomic<bool> ran{false};
    std::error_code rejected_with;
    {
        tsched::ThreadPool pool{2};
        pool.stop();
        try {
            (void)pool.submit([&ran] { ran.store(true); });
        } catch (const std::system_error& error) {
            rejected_with = error.code();
        }
    }  // Destroyed here, so a task it had kept anyway would have had its chance to run.

    EXPECT_EQ(rejected_with, std::make_error_code(std::errc::operation_canceled))
        << "submit() after stop() did not throw operation_canceled";
    EXPECT_FALSE(ran.load()) << "the rejected task ran anyway";
}

}  // namespace
