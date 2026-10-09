#pragma once

#include "tsched/detail/task.hpp"
#include "tsched/work_stealing_deque.hpp"

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace tsched {

// Settings for a ThreadPool beyond its size and hooks, for example
// ThreadPool pool{8, {}, {.stealing = false}}. At namespace scope rather than
// nested in ThreadPool: a nested struct with default member initializers can't
// be used as a default argument inside the enclosing class (CWG 1397), and both
// g++ and clang++ reject it.
struct PoolOptions {
    // Whether a worker with nothing of its own takes work queued on other
    // workers' deques. On by default. Off makes each task wait for the worker it
    // was routed to, even while others sit idle; that exists for benchmarks
    // comparing the two, and for tests that need to observe routing.
    bool stealing = true;
};

// Fixed-size pool of worker threads, each owning its own work queue.
class ThreadPool {
public:
    // Called as each worker starts and finishes, for thread naming, affinity or
    // thread-local setup. Both run *on the worker thread itself*, never on the
    // thread that built the pool. A hook that throws terminates the process:
    // it escapes the thread's entry point, which the pool does not intercept.
    struct Hooks {
        std::function<void(std::size_t index)> on_worker_start;
        std::function<void(std::size_t index)> on_worker_exit;
    };

    explicit ThreadPool(std::size_t thread_count, Hooks hooks = {}, PoolOptions options = {});

    // Stops and joins every worker. Destruction and an explicit stop() share one
    // teardown path, so stopped() ends up true either way. Must not run on one
    // of this pool's own workers: see stop().
    ~ThreadPool();

    // Workers hold a pointer back to the pool, so it can be neither copied nor moved.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Queues `task` to run on one of the pool's workers and returns a future for
    // its result. If the task throws, the exception is stored in the future
    // instead, and the worker carries on with other work.
    //
    // Once stop() has begun, throws std::system_error with
    // std::errc::operation_canceled instead: the task is not queued and never
    // runs.
    // TODO(S3-9): keep accepting submissions from the pool's own workers while
    // it drains.
    // TODO(S3-9a): if queuing fails after the task's slot is reserved, the
    // reservation is not yet released, and stop() then never returns.
    template <typename F>
        requires std::move_constructible<std::decay_t<F>> && std::invocable<std::decay_t<F>&>
    std::future<std::invoke_result_t<std::decay_t<F>&>> submit(F&& task) {
        using R = std::invoke_result_t<std::decay_t<F>&>;
        std::packaged_task<R()> work{std::forward<F>(task)};
        std::future<R> result = work.get_future();
        // Fully built before enqueue() reserves anything, so a failure up to
        // here leaves the pool untouched.
        enqueue(detail::Task{std::move(work)});
        return result;
    }

    // Number of worker threads in the pool.
    std::size_t size() const noexcept;

    // Asks every worker to stop and joins them, after the workers have run every
    // task already queued. Synchronous: once it returns, every task accepted
    // before it began has finished, no worker is still running and every
    // on_worker_exit hook has already run. From the moment it begins, submit()
    // rejects new work (see submit()).
    // Safe to call more than once; later calls do nothing. The destructor stops
    // the pool too, so calling this is optional.
    //
    // Calling it from one of this pool's own workers (for example from a hook)
    // throws std::system_error with std::errc::resource_deadlock_would_occur,
    // the error std::thread::join() uses for joining oneself, and has no effect:
    // nothing is stopped or joined. Stopping a *different* pool from a worker is
    // fine. Destroying the pool from one of its own workers terminates the
    // process, because the destructor calls stop() and cannot throw.
    void stop();

    // False while the pool is running, true once stop() has completed. Safe to
    // call from any thread. True implies every worker has been joined.
    bool stopped() const noexcept;

private:
    void enqueue(detail::Task task);
    void worker_loop(std::stop_token stop_token, std::size_t index);

    Hooks hooks_;
    std::size_t thread_count_;
    bool stealing_;  // PoolOptions::stealing; fixed for the pool's lifetime
    std::once_flag stop_once_;
    std::atomic<bool> stopped_{false};

    // Waking. queued_[i] counts the tasks reserved on worker i's deque and not
    // yet taken by anyone; total_queued_ is their sum. With stealing on, a worker
    // sleeps until some deque has work, since it can take from any of them; with
    // it off, until its own deque does. accepting_ turns false when stop()
    // begins, and from then on enqueue() reserves nothing. Never held together
    // with a deque's lock.
    std::mutex wake_mutex_;
    std::condition_variable_any wake_cv_;
    std::vector<std::size_t> queued_;  // guarded by wake_mutex_
    std::size_t total_queued_ = 0;     // guarded by wake_mutex_
    bool accepting_ = true;            // guarded by wake_mutex_

    // Spreads submissions round-robin across the workers' deques.
    std::atomic<std::size_t> next_external_{0};

    // One per worker, by index. Held by pointer because a deque can be neither
    // copied nor moved.
    std::vector<std::unique_ptr<WorkStealingDeque<detail::Task>>> deques_;

    // Declared last so it is destroyed first: every worker is stopped and joined
    // before the state above (which workers read) goes away.
    std::vector<std::jthread> threads_;
};

}  // namespace tsched
