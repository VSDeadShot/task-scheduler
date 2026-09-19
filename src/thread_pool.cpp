#include "tsched/thread_pool.hpp"

#include <algorithm>

namespace tsched {

// Slice 1 in progress: records the count only. No threads are created yet.
// A zero count is clamped to one: a pool with no workers could never run a task.
ThreadPool::ThreadPool(std::size_t thread_count)
    : thread_count_(std::max<std::size_t>(thread_count, 1)) {}

std::size_t ThreadPool::size() const noexcept { return thread_count_; }

}  // namespace tsched
