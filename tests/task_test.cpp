#include "tsched/detail/task.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using tsched::detail::Task;

// A task runs once, so it can be moved but never copied: a copy could run the
// same work twice. Moves must not throw, so handing a Task to a queue can only
// fail by running out of memory, never halfway through a move.
static_assert(!std::is_copy_constructible_v<Task>, "Task must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<Task>, "Task must not be copy-assignable");
static_assert(std::is_nothrow_move_constructible_v<Task>, "Task moves must not throw");

// Whether the future already holds a result, by name, so a failed check reads
// "timeout" rather than the raw bytes GoogleTest prints for std::future_status.
std::string status_now(const std::future<int>& result) {
    switch (result.wait_for(std::chrono::seconds(0))) {
        case std::future_status::ready:
            return "ready";
        case std::future_status::timeout:
            return "timeout";
        case std::future_status::deferred:
            return "deferred";
    }
    return "unknown";
}

// Building and moving a Task must not run it; running it runs the callable once.
TEST(Task, RunInvokesTheCallableExactlyOnce) {
    int calls = 0;
    Task task{[&calls] { ++calls; }};
    Task moved{std::move(task)};
    EXPECT_EQ(calls, 0) << "constructing or moving the Task ran the callable";

    std::move(moved).run();
    EXPECT_EQ(calls, 1);
}

// The pool's tasks are packaged_tasks: the result reaches the caller through the
// future, and only once the Task has run.
TEST(Task, DeliversAPackagedTaskValueThroughItsFuture) {
    std::packaged_task<int()> work{[] { return 42; }};
    std::future<int> result = work.get_future();
    Task task{std::move(work)};
    EXPECT_EQ(status_now(result), "timeout") << "the result was ready before run()";

    std::move(task).run();
    ASSERT_EQ(status_now(result), "ready");
    EXPECT_EQ(result.get(), 42);
}

// A throwing packaged_task stores its exception in the future, so run() itself
// returns normally; the worker running it is never the one that sees the throw.
TEST(Task, DeliversAPackagedTaskExceptionThroughItsFuture) {
    std::packaged_task<int()> work{[]() -> int { throw std::runtime_error("boom"); }};
    std::future<int> result = work.get_future();
    Task task{std::move(work)};

    EXPECT_NO_THROW(std::move(task).run());
    ASSERT_EQ(status_now(result), "ready");
    try {
        result.get();
        FAIL() << "get() returned instead of rethrowing";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string{error.what()}, "boom");
    }
}

}  // namespace
