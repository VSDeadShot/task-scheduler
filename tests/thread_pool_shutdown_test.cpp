// Shutdown under contention: what happens to submissions that race stop().
#include "test_support.hpp"
#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
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

// Calls pool->stop() from `stoppers` threads at once (released together by a
// latch) and waits up to `bound` for all of them to return, so a stop() that
// never returns fails the test instead of hanging it. Returns true if they all
// returned; every stopper has then been joined.
//
// Returns false otherwise, and lets them all go, as stop_within() does: the
// stoppers are detached and the pool is leaked (left null here). The same rules
// apply: nothing the stoppers, hooks or tasks touch may be on the caller's
// stack, and the test must not read task results after a false.
//
// `before_stop(i)` runs on stopper i immediately before it calls stop(), and
// `after_stop(i)` immediately after its own stop() returns. Each stopper runs
// its own copy of them.
[[nodiscard]] bool concurrent_stops_within(std::unique_ptr<tsched::ThreadPool>& pool, int stoppers,
                                           std::chrono::milliseconds bound, std::function<void(int)> before_stop,
                                           std::function<void(int)> after_stop) {
    struct Shared {
        explicit Shared(int count) : go(count), returned(static_cast<std::size_t>(count)) {}
        std::latch go;
        std::vector<std::promise<void>> returned;
    };
    auto shared = std::make_shared<Shared>(stoppers);
    std::vector<std::future<void>> done;
    for (std::promise<void>& returned : shared->returned) {
        done.push_back(returned.get_future());
    }
    std::vector<std::thread> threads;
    for (int i = 0; i < stoppers; ++i) {
        threads.emplace_back([target = pool.get(), shared, i, before_stop, after_stop] {
            shared->go.arrive_and_wait();
            if (before_stop) {
                before_stop(i);
            }
            target->stop();
            if (after_stop) {
                after_stop(i);
            }
            shared->returned[static_cast<std::size_t>(i)].set_value();
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + bound;
    bool all_returned = true;
    for (std::future<void>& returned : done) {
        if (returned.wait_until(deadline) != std::future_status::ready) {
            all_returned = false;
        }
    }
    if (all_returned) {
        for (std::thread& thread : threads) {
            thread.join();
        }
        return true;
    }
    for (std::thread& thread : threads) {
        thread.detach();
    }
    static_cast<void>(pool.release());
    return false;
}

// How long each task in the test below keeps its worker busy, so the drain lasts
// long enough for the second stop() caller to be waiting while tasks still run.
// Spun, not slept: a sleep this short overshoots by tens of microseconds. At
// namespace scope so a task can use it without capturing it.
constexpr auto kTaskSpin = std::chrono::microseconds(20);

constexpr int kManySubmitters = 4;
constexpr int kTwoStoppers = 2;

// How long a rejected submitter in the test below waits for the others to leave
// their submit loops before its cross probe; longer than kGiveUpAfter, so a
// submitter still being accepted has given up by then. At namespace scope so
// the submitter can use it without capturing it.
constexpr auto kProbeWaitBound = std::chrono::seconds(3);

// One round of the test below; on the heap for the same reason as
// SubmitRaceRound.
struct ManySubmitRound {
    std::atomic<int> accepted{0};    // submit() calls that returned a future, all submitters
    std::atomic<int> ran{0};         // tasks that ran
    std::atomic<int> rejections{0};  // submitters turned away so far
    std::atomic<int> done{0};        // submitters that have left their submit loop, for any reason
    std::atomic<bool> stop_over{false};
    // When stop() actually began: the first stopper to enter stamps it, then
    // stop_entered is set.
    std::atomic<std::chrono::steady_clock::rep> stop_entered_at{0};
    std::atomic<bool> stop_entered{false};
    std::latch submitters_go{kManySubmitters};

    // Slot i written only by stopper i; read once all stoppers have returned.
    std::array<std::chrono::steady_clock::time_point, kTwoStoppers> entered{};
    std::array<std::chrono::steady_clock::time_point, kTwoStoppers> returned{};
    std::array<int, kTwoStoppers> ran_at_return{};
    std::array<bool, kTwoStoppers> stopped_at_return{};

    // Written by submitter i only; read once it has been joined.
    struct Submitter {
        std::vector<std::future<void>> results;
        bool rejected = false;
        std::error_code rejected_with;
        bool other_exception = false;
        bool accepted_after_rejection = false;  // its own next submit() after its rejection was accepted
        bool began_after_others_rejection = false;     // a submit() begun after another submitter's rejection
        bool accepted_after_others_rejection = false;  // ... and that submit() was accepted
        bool probe_skipped = false;  // rejected, but the others weren't all done within kProbeWaitBound
        bool accepted_after_stop = false;
        bool gave_up = false;
        bool accepted_after_stop_entered = false;
        std::chrono::steady_clock::duration latest_accept_after_entered{0};
    };
    std::array<Submitter, kManySubmitters> submitters;
};

// Several outside submitters racing several stop() callers: every submission is
// either accepted and run before any stop() returns, or rejected with
// operation_canceled and never run. Once any submitter has been turned away, a
// submission begun after that is turned away too, whoever makes it. And each
// stop() caller, not just the one that does the work, returns only once the
// drain is over: every task that will ever run has run, and stopped() is true.
//
// Each round, a fresh 4-worker pool and four outside threads, released
// together, each submitting as fast as it can until it is turned away, exactly
// as in OutsideSubmitRacingStopIsRunOrRejected (including the one more submit
// after a rejection, and giving up kGiveUpAfter after stop() began). A rejected
// submitter then waits until all four have left their submit loops and makes
// one cross probe: a submit() begun after every other rejection, which must be
// rejected too. Without it, a submit() begins after another submitter's
// rejection only when one that was in flight across stop() returns just after
// that rejection, which some runs see in barely 10% of rounds. Each task spins
// for kTaskSpin, so a backlog takes a while to drain. Once k submissions have
// been accepted in all, k cycling through 4, 16, 64 and 256, two threads call
// stop() together. The rounds end at the first one that fails a check.
//
// Three guards make sure the races were really run, each counted in rounds:
// entered_overlap_rounds, a submit() accepted after stop() began (at least
// 10%); stopper_overlap_rounds, both stop() callers inside stop() at the same
// time (at least 50%); cross_checked_rounds, a submit() begun after another
// submitter had been turned away (at least kCrossCheckedMin).
TEST_P(ThreadPoolShutdown, ManySubmittersRacingTwoStopsAreEachRunOrRejected) {
    constexpr int kRounds = 200;
    // With the cross probe, every round of every run measured was cross-checked
    // (200 of 200, both builds, both modes). Without it, the most seen was 150.
    // Set between the two, so a probe that stops running fails this guard.
    constexpr int kCrossCheckedMin = kRounds * 7 / 8;
    constexpr int kWorkers = 4;
    constexpr std::array<int, 4> kStagger{4, 16, 64, 256};
    constexpr int kMaxAttempts = 100'000;
    constexpr auto kStaggerBound = std::chrono::seconds(2);
    constexpr auto kStopBound = std::chrono::seconds(10);

    int rounds_run = 0;
    int entered_overlap_rounds = 0;
    int stopper_overlap_rounds = 0;
    int cross_checked_rounds = 0;
    int probe_skipped_rounds = 0;
    std::chrono::steady_clock::duration max_accept_after_entered{0};
    long long accepted_total = 0;
    int bad_round = -1;
    std::string bad_checks;
    int bad_stranded = 0;

    for (int round = 0; round < kRounds; ++round) {
        auto pool = std::make_unique<tsched::ThreadPool>(kWorkers, tsched::ThreadPool::Hooks{},
                                                         tsched::PoolOptions{.stealing = GetParam()});
        auto state = std::make_shared<ManySubmitRound>();

        std::vector<std::jthread> submitters;
        submitters.reserve(kManySubmitters);
        for (int s = 0; s < kManySubmitters; ++s) {
            submitters.emplace_back([target = pool.get(), state, s] {
                ManySubmitRound::Submitter& me = state->submitters[static_cast<std::size_t>(s)];
                const auto submit_one = [target, state] {
                    return target->submit([state] {
                        const auto until = std::chrono::steady_clock::now() + kTaskSpin;
                        while (std::chrono::steady_clock::now() < until) {
                        }
                        state->ran.fetch_add(1);
                    });
                };
                state->submitters_go.arrive_and_wait();
                // The submit loop; each return leaves it.
                [&] {
                    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                        // Both read before submitting. This submitter leaves the
                        // loop once it is rejected, so any rejection counted here
                        // is another submitter's.
                        const bool after_stop = state->stop_over.load();
                        const bool others_rejected = state->rejections.load() > 0;
                        if (others_rejected) {
                            me.began_after_others_rejection = true;
                        }
                        try {
                            me.results.push_back(submit_one());
                        } catch (const std::system_error& error) {
                            state->rejections.fetch_add(1);
                            me.rejected = true;
                            me.rejected_with = error.code();
                            try {
                                me.results.push_back(submit_one());
                                state->accepted.fetch_add(1);
                                me.accepted_after_rejection = true;
                            } catch (...) {
                            }
                            return;
                        } catch (...) {
                            me.other_exception = true;
                            return;
                        }
                        state->accepted.fetch_add(1);
                        if (others_rejected) {
                            me.accepted_after_others_rejection = true;
                            return;
                        }
                        if (state->stop_entered.load()) {
                            const auto entered = std::chrono::steady_clock::time_point{
                                std::chrono::steady_clock::duration{state->stop_entered_at.load()}};
                            const auto delay = std::chrono::steady_clock::now() - entered;
                            me.accepted_after_stop_entered = true;
                            me.latest_accept_after_entered = std::max(me.latest_accept_after_entered, delay);
                            if (delay >= kGiveUpAfter) {
                                me.gave_up = true;
                                return;
                            }
                        }
                        if (after_stop) {
                            me.accepted_after_stop = true;
                            return;
                        }
                    }
                }();
                state->done.fetch_add(1);
                if (!me.rejected) {
                    return;
                }
                // The cross probe: once every submitter has left its loop,
                // which for a correct pool means every one has been turned
                // away, one more submit(). It begins after all the others'
                // rejections, so it must be rejected too.
                const auto probe_deadline = std::chrono::steady_clock::now() + kProbeWaitBound;
                while (state->done.load() < kManySubmitters) {
                    if (std::chrono::steady_clock::now() > probe_deadline) {
                        me.probe_skipped = true;
                        return;
                    }
                    std::this_thread::yield();
                }
                const bool others_rejected = state->rejections.load() > 1;  // this submitter's own is one
                if (others_rejected) {
                    me.began_after_others_rejection = true;
                }
                try {
                    me.results.push_back(submit_one());
                    state->accepted.fetch_add(1);
                    if (others_rejected) {
                        me.accepted_after_others_rejection = true;
                    } else {
                        me.accepted_after_rejection = true;
                    }
                } catch (const std::system_error& error) {
                    if (error.code() != std::make_error_code(std::errc::operation_canceled)) {
                        me.other_exception = true;  // reported as bad_code
                    }
                } catch (...) {
                    me.other_exception = true;
                }
            });
        }

        bool stagger_timeout = false;
        const int stagger = kStagger[static_cast<std::size_t>(round) % kStagger.size()];
        const auto stagger_deadline = std::chrono::steady_clock::now() + kStaggerBound;
        while (state->accepted.load() < stagger) {
            if (std::chrono::steady_clock::now() > stagger_deadline) {
                stagger_timeout = true;
                break;
            }
            std::this_thread::yield();
        }
        const bool returned = concurrent_stops_within(
            pool, kTwoStoppers, kStopBound,
            [state](int i) {
                const auto now = std::chrono::steady_clock::now();
                state->entered[static_cast<std::size_t>(i)] = now;
                // The first stopper in sets the time; the other leaves it.
                std::chrono::steady_clock::rep unset = 0;
                state->stop_entered_at.compare_exchange_strong(unset, now.time_since_epoch().count());
                state->stop_entered.store(true);
            },
            [state, target = pool.get()](int i) {
                const auto slot = static_cast<std::size_t>(i);
                state->ran_at_return[slot] = state->ran.load();
                state->stopped_at_return[slot] = target->stopped();
                state->returned[slot] = std::chrono::steady_clock::now();
            });
        state->stop_over.store(true);
        submitters.clear();  // joins them
        if (!returned) {
            bad_round = round;
            bad_checks = " stop_hung";
            break;
        }
        ++rounds_run;

        // Every stop() has returned, so no task runs from here on.
        const int final_ran = state->ran.load();
        int stranded = 0;
        bool bad_code = false;
        bool never_rejected = false;
        bool unstuck = false;
        bool cross_unstuck = false;
        bool accepted_after_entered = false;
        bool cross_checked = false;
        bool probe_skipped = false;
        for (ManySubmitRound::Submitter& me : state->submitters) {
            for (std::future<void>& result : me.results) {
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
            bad_code = bad_code || me.other_exception ||
                       (me.rejected && me.rejected_with != std::make_error_code(std::errc::operation_canceled));
            never_rejected = never_rejected || !me.rejected || me.accepted_after_stop || me.gave_up;
            unstuck = unstuck || me.accepted_after_rejection;
            cross_unstuck = cross_unstuck || me.accepted_after_others_rejection;
            accepted_after_entered = accepted_after_entered || me.accepted_after_stop_entered;
            cross_checked = cross_checked || me.began_after_others_rejection;
            probe_skipped = probe_skipped || me.probe_skipped;
            max_accept_after_entered = std::max(max_accept_after_entered, me.latest_accept_after_entered);
        }
        bool early_return = false;
        bool stopped_false = false;
        for (int i = 0; i < kTwoStoppers; ++i) {
            const auto slot = static_cast<std::size_t>(i);
            early_return = early_return || state->ran_at_return[slot] != final_ran;
            stopped_false = stopped_false || !state->stopped_at_return[slot];
        }
        accepted_total += state->accepted.load();
        if (accepted_after_entered) {
            ++entered_overlap_rounds;
        }
        if (cross_checked) {
            ++cross_checked_rounds;
        }
        if (probe_skipped) {
            ++probe_skipped_rounds;
        }
        if (std::max(state->entered[0], state->entered[1]) < std::min(state->returned[0], state->returned[1])) {
            ++stopper_overlap_rounds;
        }

        std::string failed;
        if (stranded > 0) {
            failed += " stranded";
        }
        if (final_ran != state->accepted.load()) {
            failed += " miscounted";
        }
        if (bad_code) {
            failed += " bad_code";
        }
        if (never_rejected) {
            failed += " never_rejected";
        }
        if (unstuck) {
            failed += " unstuck";
        }
        if (cross_unstuck) {
            failed += " cross_unstuck";
        }
        if (early_return) {
            failed += " early_return";
        }
        if (stopped_false) {
            failed += " stopped_false";
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
    RecordProperty("entered_overlap_rounds", entered_overlap_rounds);
    RecordProperty("stopper_overlap_rounds", stopper_overlap_rounds);
    RecordProperty("cross_checked_rounds", cross_checked_rounds);
    RecordProperty("probe_skipped_rounds", probe_skipped_rounds);
    RecordProperty("max_accept_after_stop_entered_us",
                   static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(max_accept_after_entered).count()));
    RecordProperty("accepted_total", static_cast<int>(accepted_total));
    RecordProperty("bad_round", bad_round);
    RecordProperty("bad_stranded", bad_stranded);

    // As in the test above, plus:
    // cross_unstuck: a submit() begun after another submitter was turned away
    //   was accepted.
    // early_return: a stop() caller returned while tasks were still to run.
    // stopped_false: stopped() was false right after a stop() caller returned.
    ASSERT_EQ(bad_round, -1) << "round " << bad_round << " of " << kRounds << " failed:" << bad_checks
                             << " (stranded tasks: " << bad_stranded << "); the rounds after it were skipped";
    EXPECT_GE(entered_overlap_rounds, kRounds / 10)
        << "too few rounds in which a submit() was accepted after stop() began: the race wasn't exercised";
    EXPECT_GE(stopper_overlap_rounds, kRounds / 2)
        << "too few rounds in which both stop() callers were inside stop() at once";
    EXPECT_GE(cross_checked_rounds, kCrossCheckedMin)
        << "too few rounds in which a submit() began after another submitter had been turned away";
}

}  // namespace
