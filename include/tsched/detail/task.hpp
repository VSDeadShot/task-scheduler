#pragma once

#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace tsched::detail {

// One unit of work for the pool: any callable taking no arguments, including a
// move-only one (the pool stores std::packaged_task), behind one interface.
// Move-only, because a copy could run the same work twice.
class Task {
public:
    template <typename F>
        requires(!std::same_as<std::remove_cvref_t<F>, Task>) && std::move_constructible<std::decay_t<F>> &&
                std::invocable<std::decay_t<F>&>
    explicit Task(F&& callable)
        : callable_(std::make_unique<Holder<std::decay_t<F>>>(std::forward<F>(callable))) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // Runs the callable once and consumes the Task: the callable, and anything it
    // captured, is destroyed before run() returns. Whatever the callable throws
    // propagates; a packaged_task never throws here, it stores the exception in
    // its future instead.
    //
    // Precondition: the Task has not been moved from or run already.
    void run() && {
        assert(callable_ && "Task::run() on a moved-from or already-run Task");
        std::unique_ptr<Callable> callable = std::move(callable_);
        callable->invoke();
    }

private:
    struct Callable {
        virtual ~Callable() = default;
        virtual void invoke() = 0;
    };

    template <typename F>
    struct Holder final : Callable {
        template <typename G>
        explicit Holder(G&& callable) : callable(std::forward<G>(callable)) {}
        void invoke() override { std::invoke(callable); }
        F callable;
    };

    std::unique_ptr<Callable> callable_;
};

}  // namespace tsched::detail
