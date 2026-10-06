#include "tsched/thread_pool.hpp"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

namespace tsched {

namespace {

// The pool whose worker_loop is running on this thread, if any, and this
// worker's index in it. Lets stop() recognise a call from one of its own
// workers, and lets enqueue() keep a worker's own submissions on its deque.
// The index means nothing to any other pool.
thread_local const ThreadPool* current_worker_pool = nullptr;
thread_local std::size_t current_worker_index = 0;

}  // namespace

ThreadPool::ThreadPool(std::size_t thread_count, Hooks hooks)
    : hooks_(std::move(hooks)),
      thread_count_(std::max<std::size_t>(thread_count, 1)),
      queued_(thread_count_, 0) {
    // Every deque exists before the first worker starts.
    deques_.reserve(thread_count_);
    for (std::size_t index = 0; index < thread_count_; ++index) {
        deques_.push_back(std::make_unique<WorkStealingDeque<detail::Task>>());
    }
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

void ThreadPool::enqueue(detail::Task task) {
    // One of this pool's own workers keeps its submissions on its own deque.
    // Everyone else, including a worker of some other pool (whose index belongs
    // to that pool, and may not even be in range here), goes round-robin.
    // Relaxed is enough: the counter only spreads work. The reservation and
    // push below are what make the task visible to its worker.
    const std::size_t target = current_worker_pool == this
                                   ? current_worker_index
                                   : next_external_.fetch_add(1, std::memory_order_relaxed) % thread_count_;
    assert(target < deques_.size());
    // Reserve, then push: a worker woken by the reservation may find its deque
    // still empty for a moment, and simply looks again. The two locks are taken
    // one after the other, never together.
    {
        std::lock_guard lock{wake_mutex_};
        ++queued_[target];
    }
    deques_[target]->push(std::move(task));
    // All workers share one condition variable, and only the target can take
    // this task, so a single notify could wake the wrong one.
    wake_cv_.notify_all();
}

void ThreadPool::worker_loop(std::stop_token stop_token, std::size_t index) {
    current_worker_pool = this;
    current_worker_index = index;

    if (hooks_.on_worker_start) {
        hooks_.on_worker_start(index);
    }

    while (true) {
        if (std::optional<detail::Task> task = deques_[index]->pop()) {
            {
                std::lock_guard lock{wake_mutex_};
                --queued_[index];
            }
            std::move(*task).run();
            continue;
        }
        // Sleep until this worker has work of its own or stop is requested. The
        // count is checked under the same lock enqueue() takes to raise it, so a
        // reservation can never slip in between the check and the sleep. The
        // stop_token wakes the wait when stop is requested, with no notify.
        std::unique_lock lock{wake_mutex_};
        const bool has_work = wake_cv_.wait(lock, stop_token, [&] { return queued_[index] > 0; });
        // TODO(S3-8): drain instead. Until then a worker leaves as soon as stop is
        // requested, even with work queued: those tasks never run, and their
        // futures report broken_promise once the pool (and its deques) is destroyed.
        if (!has_work || stop_token.stop_requested()) {
            break;
        }
    }

    if (hooks_.on_worker_exit) {
        hooks_.on_worker_exit(index);
    }
}

}  // namespace tsched
