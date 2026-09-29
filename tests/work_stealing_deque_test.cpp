#include "tsched/work_stealing_deque.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>

// Static check: instantiating every member with a move-only T fails the build if
// any of them copies an item. Tasks in Slice 3 (std::packaged_task) are move-only.
template class tsched::WorkStealingDeque<std::unique_ptr<int>>;

namespace {

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

}  // namespace
