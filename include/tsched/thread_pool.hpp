#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace tsched {

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

    explicit ThreadPool(std::size_t thread_count, Hooks hooks = {});

    // Workers hold a pointer back to the pool, so it can be neither copied nor moved.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Number of worker threads in the pool.
    std::size_t size() const noexcept;

    // Asks every worker to stop and joins them. Synchronous: once it returns,
    // no worker is still running and every on_worker_exit hook has already run.
    // Safe to call more than once; later calls do nothing. The destructor stops
    // the pool too, so calling this is optional.
    //
    // Must NOT be called from a worker thread: a worker would join itself.
    // Guarding against that is a future behavior, not handled here.
    void stop();

private:
    void worker_loop(std::stop_token stop_token, std::size_t index);

    Hooks hooks_;
    std::size_t thread_count_;
    std::once_flag stop_once_;

    // Declared last so it is destroyed first: every worker is stopped and joined
    // before the state above (which workers read) goes away.
    std::vector<std::jthread> threads_;
};

}  // namespace tsched
