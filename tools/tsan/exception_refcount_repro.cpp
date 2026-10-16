// Minimal reproduction of a ThreadSanitizer false positive in libstdc++'s
// exception reference counting. No thread pool code is involved: one thread
// stores an exception in a std::promise, and the main thread catches it from
// the future and reads its message. The program is correct; under TSan it
// still reports two data races. See README.md in this directory for the
// cause, how to build and run this, and what to expect.
//
// Not part of the CMake build on purpose: its job is to make TSan report, so
// it must never run under ctest.

#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

int main() {
    constexpr int kRounds = 200;
    int wrong_messages = 0;
    for (int round = 0; round < kRounds; ++round) {
        std::promise<int> promise;
        std::future<int> result = promise.get_future();
        std::thread storer{[promise = std::move(promise)]() mutable {
            promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
            // Lets the main thread catch the exception and finish with it
            // first. A sleep gives TSan no ordering between the two threads.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // Releases this thread's reference to the shared state, and with
            // it the last reference to the exception, which frees it here.
            std::promise<int> released = std::move(promise);
        }};
        try {
            result.get();
        } catch (const std::runtime_error& error) {
            if (std::string{error.what()} != "boom") {
                ++wrong_messages;
            }
        }
        storer.join();
    }
    // Always 0: the program is correct, whatever TSan reports.
    std::printf("rounds=%d wrong_messages=%d\n", kRounds, wrong_messages);
    return wrong_messages == 0 ? 0 : 1;
}
