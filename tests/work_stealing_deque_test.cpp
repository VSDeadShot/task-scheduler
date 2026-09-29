#include "tsched/work_stealing_deque.hpp"

#include <gtest/gtest.h>

#include <optional>

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

}  // namespace
