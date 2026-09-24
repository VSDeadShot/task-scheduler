#include "tsched/thread_pool.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <utility>

namespace tsched {

namespace {

// The pool whose worker_loop is running on this thread, if any. Lets stop()
// recognise a call from one of its own workers.
thread_local const ThreadPool* current_worker_pool = nullptr;

}  // namespace

ThreadPool::ThreadPool(std::size_t thread_count, Hooks hooks)
    : hooks_(std::move(hooks)), thread_count_(std::max<std::size_t>(thread_count, 1)) {
    threads_.reserve(thread_count_);
    for (std::size_t index = 0; index < thread_count_; ++index) {
        threads_.emplace_back(
            [this, index](std::stop_token stop_token) { worker_loop(std::move(stop_token), index); });
    }
}

ThreadPool::~ThreadPool() { stop(); }

std::size_t ThreadPool::size() const noexcept { return thread_count_; }

void ThreadPool::stop() {
    // Must be checked here, before call_once. Inside it, a worker would only fail
    // at its own join(), after stopping and joining every worker before it; and
    // if another thread were already inside stop(), the worker would block in
    // call_once waiting for a call that is itself waiting to join that worker.
    // An exception escaping call_once's callable is also unsafe under TSan, whose
    // runtime never resets the flag, so the next stop() would hang.
    if (current_worker_pool == this) {
        throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur),
                                "ThreadPool::stop() called from one of its own workers");
    }
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
    current_worker_pool = this;

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
