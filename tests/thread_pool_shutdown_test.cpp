// Shutdown under contention: what happens to submissions that race stop().
#include "test_support.hpp"
#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using tsched_test::stealing_param_name;
using tsched_test::stop_within;

// Run with stealing on and off. With it off, a task accepted onto the deque of
// a worker that has already left is never taken by anyone else, so a submission
// accepted too late is stranded even while other workers are still draining.
class ThreadPoolShutdown : public ::testing::TestWithParam<bool> {};

INSTANTIATE_TEST_SUITE_P(, ThreadPoolShutdown, ::testing::Bool(), stealing_param_name);

// One round of the race below. On the heap and co-owned by the submitter and by
// every task, so that if stop() never returns and the pool is leaked, nothing
// they touch is on the test's stack.
struct SubmitRaceRound {
    std::atomic<int> accepted{0};          // submit() calls that returned a future
    std::atomic<int> ran{0};               // tasks that ran
    std::atomic<bool> stop_called{false};  // set by the test thread just before stop_within()
    std::atomic<bool> stop_over{false};    // set once stop() has returned (or was given up on)
    // When stop() actually began: stamped by the stopper thread immediately
    // before it calls stop(), then stop_entered is set.
    std::atomic<std::chrono::steady_clock::rep> stop_entered_at{0};
    std::atomic<bool> stop_entered{false};

    // Written by the submitter only; read once it has been joined.
    std::vector<std::future<void>> results;
    bool rejected = false;
    std::error_code rejected_with;
    bool other_exception = false;           // submit() threw something other than system_error
    bool accepted_after_rejection = false;  // the submit() right after a rejection was accepted
    bool accepted_after_stop = false;       // a submit() begun after stop() returned was accepted
    bool gave_up = false;                   // still being accepted kGiveUpAfter after stop() began
    int accepted_after_stop_called = 0;     // accepted submit()s that returned after stop_called was set
    int accepted_after_stop_entered = 0;    // accepted submit()s that returned after stop() began
    std::chrono::steady_clock::duration latest_accept_after_entered{0};  // the longest such delay
};

// How long after stop() began a submission may still be accepted before the
// submitter stops trying and the round counts as never rejected. A correct pool
// stops accepting as soon as stop() takes its lock, so only a submission that
// reserved just before that can return later, within microseconds unless its
// thread is preempted. At namespace scope so the submitter can use it without
// capturing it.
constexpr auto kGiveUpAfter = std::chrono::seconds(1);

// A submit() from outside the pool that races stop() is either accepted, and
// then its task runs before stop() returns, or rejected with operation_canceled
// and its task never runs. It is never accepted and then left where no worker
// will take it. Once one is rejected, so is the next.
//
// Each round, a fresh 2-worker pool and one outside thread submitting as fast as
// it can until it is turned away. The test thread waits until that thread has
// had k submissions accepted, k cycling through 1, 4, 16 and 64, and only then
// calls stop() (bounded, see stop_within()), so stop() always begins while
// submissions are in flight and there is accepted work to drain. Without that
// stagger, stop() usually wins before the first submit() in a plain build.
//
// A pool that never turns outside submissions away can keep stop() from
// returning: with stealing on, a submitter that keeps both workers fed means
// neither ever sees nothing queued, so neither leaves. So the submitter gives up
// once a submission is accepted kGiveUpAfter or more after stop() actually
// began, and the round counts as never rejected. The clock starts when the
// stopper thread is about to call stop(), not when the test asks for it: the
// stopper thread can take milliseconds to be scheduled, and a correct pool
// accepts thousands of submissions in that time.
// max_accept_after_stop_entered_us records the longest delay a correct pool
// actually shows.
//
// The rounds end at the first one that fails a check, which is reported with
// the checks it failed, so a broken pool fails fast on a value instead of
// running into the ctest timeout.
//
// entered_overlap_rounds counts rounds in which a submit() was still accepted
// after stop() actually began: rounds where acceptance and stop() were really
// contested. It must be at least 10% of rounds, or the test is not exercising
// the race it is about. overlap_rounds, the same count from when the test
// thread asked for the stop, is only recorded: it includes the stopper thread's
// scheduling delay, so it sits near 100% whatever the pool does.
TEST_P(ThreadPoolShutdown, OutsideSubmitRacingStopIsRunOrRejected) {
    constexpr int kRounds = 500;
    constexpr int kWorkers = 2;
    constexpr std::array<int, 4> kStagger{1, 4, 16, 64};
    constexpr int kMaxAttempts = 100'000;
    constexpr auto kStaggerBound = std::chrono::seconds(2);
    constexpr auto kStopBound = std::chrono::seconds(10);

    int rounds_run = 0;
    int overlap_rounds = 0;
    int max_accepted_after_stop_called = 0;
    // Rounds, and the longest delay, in which a submission was accepted after
    // stop() actually began: the submissions that reserved just before stop()
    // took its lock.
    int entered_overlap_rounds = 0;
    std::chrono::steady_clock::duration max_accept_after_entered{0};
    long long accepted_total = 0;
    // The first round that failed a check, and what it failed.
    int bad_round = -1;
    std::string bad_checks;
    int bad_stranded = 0;

    for (int round = 0; round < kRounds; ++round) {
        auto pool = std::make_unique<tsched::ThreadPool>(kWorkers, tsched::ThreadPool::Hooks{},
                                                         tsched::PoolOptions{.stealing = GetParam()});
        auto state = std::make_shared<SubmitRaceRound>();

        std::jthread submitter{[target = pool.get(), state] {
            const auto submit_one = [target, state] {
                return target->submit([state] { state->ran.fetch_add(1); });
            };
            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                // Read before submitting: once stop() has returned, this
                // submission must be turned away.
                const bool after_stop = state->stop_over.load();
                try {
                    state->results.push_back(submit_one());
                } catch (const std::system_error& error) {
                    state->rejected = true;
                    state->rejected_with = error.code();
                    try {
                        state->results.push_back(submit_one());
                        state->accepted.fetch_add(1);
                        state->accepted_after_rejection = true;
                    } catch (...) {
                    }
                    return;
                } catch (...) {
                    state->other_exception = true;
                    return;
                }
                state->accepted.fetch_add(1);
                if (state->stop_called.load()) {
                    ++state->accepted_after_stop_called;
                }
                if (state->stop_entered.load()) {
                    const auto entered = std::chrono::steady_clock::time_point{
                        std::chrono::steady_clock::duration{state->stop_entered_at.load()}};
                    const auto delay = std::chrono::steady_clock::now() - entered;
                    ++state->accepted_after_stop_entered;
                    state->latest_accept_after_entered = std::max(state->latest_accept_after_entered, delay);
                    if (delay >= kGiveUpAfter) {
                        state->gave_up = true;
                        return;
                    }
                }
                if (after_stop) {
                    state->accepted_after_stop = true;
                    return;
                }
            }
        }};

        bool stagger_timeout = false;
        const int stagger = kStagger[round % kStagger.size()];
        const auto stagger_deadline = std::chrono::steady_clock::now() + kStaggerBound;
        while (state->accepted.load() < stagger) {
            if (std::chrono::steady_clock::now() > stagger_deadline) {
                stagger_timeout = true;
                break;
            }
            std::this_thread::yield();
        }
        state->stop_called.store(true);
        const bool returned = stop_within(pool, kStopBound, [state] {
            state->stop_entered_at.store(std::chrono::steady_clock::now().time_since_epoch().count());
            state->stop_entered.store(true);
        });
        state->stop_over.store(true);
        submitter.join();
        if (!returned) {
            // The pool is leaked; its tasks' results can't be read safely.
            bad_round = round;
            bad_checks = " stop_hung";
            break;
        }
        ++rounds_run;

        // stop() is synchronous: every accepted task has already run, so no
        // result is waited for.
        int stranded = 0;
        for (std::future<void>& result : state->results) {
            if (!result.valid() || result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++stranded;
                continue;
            }
            try {
                result.get();
            } catch (const std::future_error&) {
                ++stranded;
            }
        }
        accepted_total += state->accepted.load();
        max_accepted_after_stop_called = std::max(max_accepted_after_stop_called, state->accepted_after_stop_called);
        max_accept_after_entered = std::max(max_accept_after_entered, state->latest_accept_after_entered);
        if (state->accepted_after_stop_called > 0) {
            ++overlap_rounds;
        }
        if (state->accepted_after_stop_entered > 0) {
            ++entered_overlap_rounds;
        }

        std::string failed;
        if (stranded > 0) {
            failed += " stranded";
        }
        if (state->ran.load() != state->accepted.load()) {
            failed += " miscounted";
        }
        if (state->other_exception ||
            (state->rejected && state->rejected_with != std::make_error_code(std::errc::operation_canceled))) {
            failed += " bad_code";
        }
        if (!state->rejected || state->accepted_after_stop || state->gave_up) {
            failed += " never_rejected";
        }
        if (state->accepted_after_rejection) {
            failed += " unstuck";
        }
        if (stagger_timeout) {
            failed += " stagger_timeout";
        }
        if (!failed.empty()) {
            bad_round = round;
            bad_checks = failed;
            bad_stranded = stranded;
            break;
        }
    }  // each round's pool is destroyed here, after its checks

    RecordProperty("rounds_run", rounds_run);
    RecordProperty("overlap_rounds", overlap_rounds);
    RecordProperty("max_accepted_after_stop_called", max_accepted_after_stop_called);
    RecordProperty("entered_overlap_rounds", entered_overlap_rounds);
    RecordProperty("max_accept_after_stop_entered_us",
                   static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(max_accept_after_entered).count()));
    RecordProperty("accepted_total", static_cast<int>(accepted_total));
    RecordProperty("bad_round", bad_round);
    RecordProperty("bad_stranded", bad_stranded);

    // stranded: accepted tasks not run, or dropped, by the time stop() returned.
    // miscounted: the tasks that ran didn't match the submissions accepted.
    // bad_code: turned away with something other than operation_canceled.
    // never_rejected: never turned away, or accepted after stop() returned.
    // unstuck: accepted right after a rejection.
    ASSERT_EQ(bad_round, -1) << "round " << bad_round << " of " << kRounds << " failed:" << bad_checks
                             << " (stranded tasks: " << bad_stranded << "); the rounds after it were skipped";
    // Vacuity guard: the race has to be contested to prove anything. Measured at
    // 70-83% of rounds in both builds and modes.
    EXPECT_GE(entered_overlap_rounds, kRounds / 10)
        << "too few rounds in which a submit() was accepted after stop() began: the race wasn't exercised";
}

}  // namespace
