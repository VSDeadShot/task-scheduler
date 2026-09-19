#pragma once

#include <cstddef>

namespace tsched {

// Fixed-size pool of worker threads, each owning its own work queue.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count);

    // Workers hold a pointer back to the pool, so it can be neither copied nor moved.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Number of worker threads in the pool.
    std::size_t size() const noexcept;

private:
    std::size_t thread_count_;
};

}  // namespace tsched
