#include "fail_alloc.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

// Replaces the global operator new/delete for the executable this file is
// linked into. It lives in its own translation unit on purpose: with the
// replacement visible in the same file as its callers, g++ inlines it and
// reports -Wmismatched-new-delete (free() on memory from operator new), which
// -Werror turns into a build failure.
//
// Under clang's ThreadSanitizer this executable must link the sanitizer runtime
// as a shared library: the static runtime defines operator new itself and is
// linked whole, so a replacement collides with it. See tests/CMakeLists.txt.
//
// Only the six plain forms are replaced. Aligned and nothrow forms keep the
// library's definitions; none of the allocations this seam targets uses them.

namespace {

// Size to fail, or 0 when disarmed; and whether it has failed since arming.
// Trivially initialized thread_locals, so reading them never allocates.
thread_local std::size_t fail_size = 0;
thread_local bool failed = false;

void* allocate(std::size_t size) {
    if (fail_size != 0 && size == fail_size) {
        fail_size = 0;  // once only
        failed = true;
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

}  // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t /*size*/) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t /*size*/) noexcept { std::free(memory); }

namespace tsched_test {

FailNextAllocationOfSize::FailNextAllocationOfSize(std::size_t size) {
    failed = false;
    fail_size = size;
}

FailNextAllocationOfSize::~FailNextAllocationOfSize() { fail_size = 0; }

bool FailNextAllocationOfSize::fired() const { return failed; }

}  // namespace tsched_test
