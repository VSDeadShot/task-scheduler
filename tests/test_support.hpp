#pragma once

// Helpers shared by the test executables. Header-only, so including it links
// nothing new into any of them.

#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <string>
#include <thread>

#include <time.h>  // clock_gettime, CLOCK_PROCESS_CPUTIME_ID (POSIX)

namespace tsched_test {

// Whether the future holds a result within `wait`, by name, so a failed check
// reads "timeout" rather than the raw bytes GoogleTest prints for
// std::future_status. Waiting with a bound means a wiring bug fails the test
// instead of hanging it. A zero `wait` only looks: it never blocks.
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

// Process CPU time (all threads, user + system), in milliseconds. Tests use it
// to tell sleeping workers from spinning ones: a worker parked on a condition
// variable is blocked in the kernel and costs ~nothing, while a spinning one
// costs about as much CPU as the window lasts. Other processes cannot bill CPU
// time to this one, so machine load cannot make a sleeping pool look busy.
//
// Limitation: this catches spinning, not slow polling. A worker that loops on
// sleep_for(1ms) costs only a millisecond or two over a 250 ms window and would
// pass. Telling "woken by notification" apart from "polling" would need
// voluntary context-switch counts (getrusage ru_nvcsw), which is out of scope.
//
// Portability: POSIX-only (CLOCK_PROCESS_CPUTIME_ID). A Windows port needs
// GetProcessTimes instead. std::clock() is not a substitute: on MSVC it returns
// wall-clock time, so a sleeping pool would appear to burn the whole window.
inline double process_cpu_ms() {
    ::timespec ts{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        ADD_FAILURE() << "clock_gettime(CLOCK_PROCESS_CPUTIME_ID) failed";
    }
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
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

// Names the two instances of a test parameterized on PoolOptions::stealing:
// .../StealingOn and .../StealingOff.
inline std::string stealing_param_name(const ::testing::TestParamInfo<bool>& info) {
    return info.param ? "StealingOn" : "StealingOff";
}

// Calls pool->stop() on another thread and waits up to `bound` for it to
// return, so a stop() that never returns fails the test instead of hanging it.
// Returns true if it returned; the stopper has then been joined.
//
// Returns false otherwise, and lets both go: the stopper is stuck joining a
// worker that will never leave, so it is detached and the pool is leaked (left
// null here) rather than destroyed, which would hang too. The process can still
// exit, and ctest runs each test in its own process, so nothing else shares it.
// The stopper holds nothing on the caller's stack: if stop() returns late after
// all, it only touches the leaked pool and a promise it shares ownership of.
// For the same reason, anything a leaked worker could still touch, such as
// state a still-queued task refers to, must be on the heap and co-owned by the
// tasks, and the test must not read task results after a false.
//
// `before_stop`, if given, runs on the stopper thread immediately before it
// calls stop(), so a test can tell when stop() actually began rather than when
// it asked for it (the stopper thread may take a while to be scheduled). It is
// moved onto the stopper thread, so it must not refer to the caller's stack.
[[nodiscard]] inline bool stop_within(std::unique_ptr<tsched::ThreadPool>& pool, std::chrono::milliseconds bound,
                                      std::function<void()> before_stop = {}) {
    auto stop_returned = std::make_shared<std::promise<void>>();
    std::future<void> stopped = stop_returned->get_future();
    std::thread stopper{[target = pool.get(), stop_returned, before_stop = std::move(before_stop)] {
        if (before_stop) {
            before_stop();
        }
        target->stop();
        stop_returned->set_value();
    }};
    if (stopped.wait_for(bound) == std::future_status::ready) {
        stopper.join();
        return true;
    }
    stopper.detach();
    static_cast<void>(pool.release());
    return false;
}

}  // namespace tsched_test
