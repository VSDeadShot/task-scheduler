#pragma once

#include <optional>

namespace tsched {

// A worker's task queue. The owning worker takes items from the front; other,
// idle workers steal from the back.
//
// There is no empty() or size(): with other threads stealing, the answer would
// be stale as soon as it returned. An empty optional from pop() or steal() is
// the only emptiness signal.
template <typename T>
class WorkStealingDeque {
public:
    // Owner only. Takes the front item, or returns nothing if the deque is empty.
    std::optional<T> pop() { return std::nullopt; }

    // Any other thread. Takes the back item, or returns nothing if the deque is empty.
    std::optional<T> steal() { return std::nullopt; }
};

}  // namespace tsched
