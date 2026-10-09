#pragma once

#include <cstddef>

// Test-only allocation-failure seam. fail_alloc.cpp replaces the global
// operator new for the one test executable that links it; every other test
// executable keeps the standard one.
namespace tsched_test {

// While alive, makes the first allocation of exactly `size` bytes on the
// calling thread throw std::bad_alloc, once. Other sizes, other threads, and
// any allocation after the one that failed are served normally. Arm it around
// a single call, so allocations made by the test itself (or by GoogleTest)
// can't be the one that fails.
class FailNextAllocationOfSize {
public:
    explicit FailNextAllocationOfSize(std::size_t size);
    ~FailNextAllocationOfSize();

    FailNextAllocationOfSize(const FailNextAllocationOfSize&) = delete;
    FailNextAllocationOfSize& operator=(const FailNextAllocationOfSize&) = delete;

    // Whether an allocation failed while this was armed: a caught bad_alloc
    // came from the seam rather than from real memory exhaustion.
    bool fired() const;
};

}  // namespace tsched_test
