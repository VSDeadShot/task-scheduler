#include "tsched/work_stealing_deque.hpp"

#include <gtest/gtest.h>

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

}  // namespace
