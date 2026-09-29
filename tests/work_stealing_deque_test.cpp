#include "tsched/work_stealing_deque.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// Static check: instantiating every member with a move-only T fails the build if
// any of them copies an item. Tasks in Slice 3 (std::packaged_task) are move-only.
template class tsched::WorkStealingDeque<std::unique_ptr<int>>;

namespace {

// A deque owns its lock, so it can be neither copied nor moved. Owners that need
// to relocate one (a pool's vector of per-worker deques) hold it by pointer.
using IntDeque = tsched::WorkStealingDeque<int>;
static_assert(!std::is_copy_constructible_v<IntDeque>, "WorkStealingDeque must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<IntDeque>, "WorkStealingDeque must not be copy-assignable");
static_assert(!std::is_move_constructible_v<IntDeque>, "WorkStealingDeque must not be move-constructible");
static_assert(!std::is_move_assignable_v<IntDeque>, "WorkStealingDeque must not be move-assignable");

// An empty optional is the deque's only emptiness signal (there is no empty()
// or size(): under concurrency the answer would be stale on return), so both
// ends must report "nothing here" rather than a default-constructed item.
TEST(WorkStealingDeque, PopOnEmptyReturnsNothing) {
    tsched::WorkStealingDeque<int> deque;
    EXPECT_FALSE(deque.pop().has_value());
}

TEST(WorkStealingDeque, StealOnEmptyReturnsNothing) {
    tsched::WorkStealingDeque<int> deque;
    EXPECT_FALSE(deque.steal().has_value());
}

// The owner's queue is FIFO: tasks run in the order they were submitted, so
// early submissions are not starved by later ones.
TEST(WorkStealingDeque, OwnerPopsInPushOrder) {
    tsched::WorkStealingDeque<int> deque;
    deque.push(1);
    deque.push(2);
    deque.push(3);
    EXPECT_EQ(deque.pop(), std::optional<int>{1});
    EXPECT_EQ(deque.pop(), std::optional<int>{2});
    EXPECT_EQ(deque.pop(), std::optional<int>{3});
}

// A thief takes the newest item from the back, the end the owner reaches last,
// so stealing does not disturb the owner's FIFO order at the front.
TEST(WorkStealingDeque, StealTakesFromTheOppositeEndToPop) {
    tsched::WorkStealingDeque<int> deque;
    deque.push(1);
    deque.push(2);
    deque.push(3);
    EXPECT_EQ(deque.steal(), std::optional<int>{3});
    EXPECT_EQ(deque.pop(), std::optional<int>{1});
}

// Counts how often it is copied and moved, into counters owned by the test.
struct CopyMoveCounter {
    struct Counts {
        int copies = 0;
        int moves = 0;
    };

    explicit CopyMoveCounter(Counts* counts) : counts_(counts) {}
    CopyMoveCounter(const CopyMoveCounter& other) : counts_(other.counts_) { ++counts_->copies; }
    CopyMoveCounter(CopyMoveCounter&& other) noexcept : counts_(other.counts_) { ++counts_->moves; }
    CopyMoveCounter& operator=(const CopyMoveCounter& other) {
        counts_ = other.counts_;
        ++counts_->copies;
        return *this;
    }
    CopyMoveCounter& operator=(CopyMoveCounter&& other) noexcept {
        counts_ = other.counts_;
        ++counts_->moves;
        return *this;
    }

    Counts* counts_;
};

// Items are moved through the deque, never copied, even when T could be copied:
// a copied task would be wasted work at best, and a move-only task (see the
// static check above) would not compile. Each operation's own copies are
// checked, so a failure names the operation that copied and no other.
TEST(WorkStealingDeque, ItemsAreMovedNeverCopied) {
    CopyMoveCounter::Counts counts;
    tsched::WorkStealingDeque<CopyMoveCounter> deque;

    deque.push(CopyMoveCounter{&counts});
    deque.push(CopyMoveCounter{&counts});
    EXPECT_EQ(counts.copies, 0) << "push() copied";

    int copies_before = counts.copies;
    std::optional<CopyMoveCounter> popped = deque.pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(counts.copies - copies_before, 0) << "pop() copied";

    copies_before = counts.copies;
    std::optional<CopyMoveCounter> stolen = deque.steal();
    ASSERT_TRUE(stolen.has_value());
    EXPECT_EQ(counts.copies - copies_before, 0) << "steal() copied";

    // Proves the counter is wired up, so zero copies is not a vacuous pass. The
    // exact move count is left to the implementation.
    EXPECT_GT(counts.moves, 0);
}

// A move-only item survives the round trip at both ends: the very object pushed
// comes back out.
TEST(WorkStealingDeque, MoveOnlyItemsRoundTrip) {
    tsched::WorkStealingDeque<std::unique_ptr<int>> deque;
    auto first = std::make_unique<int>(1);
    auto second = std::make_unique<int>(2);
    int* const first_address = first.get();
    int* const second_address = second.get();

    deque.push(std::move(first));
    deque.push(std::move(second));

    std::optional<std::unique_ptr<int>> popped = deque.pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->get(), first_address);

    std::optional<std::unique_ptr<int>> stolen = deque.steal();
    ASSERT_TRUE(stolen.has_value());
    EXPECT_EQ(stolen->get(), second_address);
}

// Lists up to the first few indices, for a readable failure message.
std::string first_indices(const std::vector<int>& indices) {
    constexpr std::size_t kShown = 10;
    std::string text;
    for (std::size_t i = 0; i < indices.size() && i < kShown; ++i) {
        text += (i == 0 ? "" : ", ") + std::to_string(indices[i]);
    }
    return indices.size() > kShown ? text + ", ..." : text;
}

// The owner pushes and pops at the front while thieves steal from the back, all
// at once. Every item must come out exactly once: none lost, none handed to two
// takers. A bug where each step is locked but a whole pop() or steal() is not
// has no data race, so ThreadSanitizer cannot see it; only these counts can.
//
// The run always ends, even with such a bug: the owner drains its end until
// pop() finds nothing, and nothing is pushed after that, so a lost item shows
// up as a count of 0 instead of a hang.
TEST(WorkStealingDeque, EachItemIsTakenExactlyOnceUnderContention) {
    constexpr int kItems = 100'000;
    constexpr int kThieves = 3;

    tsched::WorkStealingDeque<int> deque;
    std::vector<std::atomic<int>> taken(kItems);    // how often each item came out
    std::vector<int> steals_by_thief(kThieves, 0);  // one slot per thief, read after join
    std::atomic<bool> owner_done{false};
    std::latch start{kThieves + 1};

    std::vector<std::jthread> thieves;
    for (int t = 0; t < kThieves; ++t) {
        thieves.emplace_back([&, t] {
            start.arrive_and_wait();
            while (!owner_done.load()) {
                if (std::optional<int> item = deque.steal()) {
                    taken[*item].fetch_add(1);
                    ++steals_by_thief[t];
                }
            }
        });
    }

    // This thread is the owner. Popping every other step keeps the deque
    // growing, so the thieves always have something to contend for.
    int pops = 0;
    start.arrive_and_wait();
    for (int i = 0; i < kItems; ++i) {
        deque.push(i);
        if (i % 2 == 1) {
            if (std::optional<int> item = deque.pop()) {
                taken[*item].fetch_add(1);
                ++pops;
            }
        }
    }
    while (std::optional<int> item = deque.pop()) {
        taken[*item].fetch_add(1);
        ++pops;
    }
    owner_done.store(true);
    thieves.clear();  // joins

    std::vector<int> lost;
    std::vector<int> duplicated;
    for (int i = 0; i < kItems; ++i) {
        const int count = taken[i].load();
        if (count == 0) {
            lost.push_back(i);
        } else if (count > 1) {
            duplicated.push_back(i);
        }
    }
    int steals = 0;
    for (int s : steals_by_thief) {
        steals += s;
    }
    RecordProperty("pops", pops);
    RecordProperty("steals", steals);

    EXPECT_EQ(lost.size(), 0u) << "first lost: " << first_indices(lost);
    EXPECT_EQ(duplicated.size(), 0u) << "first duplicated: " << first_indices(duplicated);
    // Both ends must actually have taken items, or nothing was contended.
    EXPECT_GT(steals, 0);
    EXPECT_GT(pops, 0);
}

}  // namespace
