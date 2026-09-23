#include "tsched/thread_pool.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace tsched {

ThreadPool::ThreadPool(std::size_t thread_count, Hooks hooks)
    : hooks_(std::move(hooks)), thread_count_(std::max<std::size_t>(thread_count, 1)) {
    threads_.reserve(thread_count_);
    for (std::size_t index = 0; index < thread_count_; ++index) {
        threads_.emplace_back(
            [this, index](std::stop_token stop_token) { worker_loop(std::move(stop_token), index); });
    }
}

std::size_t ThreadPool::size() const noexcept { return thread_count_; }

void ThreadPool::stop() {
    // call_once makes this idempotent and safe from several threads at once: a
    // concurrent caller blocks here until the first call has finished joining,
    // so nobody observes a half-stopped pool or joins a thread twice.
    std::call_once(stop_once_, [this] {
        for (auto& thread : threads_) {
            thread.request_stop();
        }
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        // Set only after every join, so stopped() == true always implies no
        // worker is still running.
        stopped_.store(true);
    });
}

bool ThreadPool::stopped() const noexcept { return stopped_.load(); }

void ThreadPool::worker_loop(std::stop_token stop_token, std::size_t index) {
    if (hooks_.on_worker_start) {
        hooks_.on_worker_start(index);
    }

    // Park until stop is requested. Slice 2 replaces this with the worker's own
    // work-stealing deque; for now it keeps each worker alive and asleep rather
    // than spinning. Waiting on the stop_token means no notify is needed: the
    // jthread destructor requests stop, which wakes the wait.
    std::mutex idle_mutex;
    std::condition_variable_any idle_cv;
    std::unique_lock lock(idle_mutex);
    idle_cv.wait(lock, stop_token, [] { return false; });

    if (hooks_.on_worker_exit) {
        hooks_.on_worker_exit(index);
    }
}

}  // namespace tsched
