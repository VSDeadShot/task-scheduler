#include "tsched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

namespace {

// A pool owns running threads that refer back to it, so it must never be
// copied or moved. Checked at compile time: a violation fails the build.
static_assert(!std::is_copy_constructible_v<tsched::ThreadPool>,
              "ThreadPool must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<tsched::ThreadPool>,
              "ThreadPool must not be copy-assignable");
static_assert(!std::is_move_constructible_v<tsched::ThreadPool>,
              "ThreadPool must not be move-constructible");
static_assert(!std::is_move_assignable_v<tsched::ThreadPool>,
              "ThreadPool must not be move-assignable");

// Several counts, so a constant return value cannot pass by coincidence.
TEST(ThreadPoolLifecycle, SizeReportsRequestedThreadCount) {
    for (std::size_t requested : {1u, 3u, 8u}) {
        SCOPED_TRACE(testing::Message() << "requested=" << requested);
        tsched::ThreadPool pool{requested};
        EXPECT_EQ(pool.size(), requested);
    }
}

// A pool with no workers could never run anything, so zero is clamped up.
TEST(ThreadPoolLifecycle, ZeroThreadCountClampsToOne) {
    tsched::ThreadPool pool{0};
    EXPECT_EQ(pool.size(), 1u);
}

}  // namespace
