#pragma once

#include <deque>
#include <optional>
#include <utility>

namespace tsched {

// A worker's task queue. The owning worker pushes to the back and takes from
// the front, so its own tasks run in FIFO order; other, idle workers steal
// from the back.
//
// There is no empty() or size(): with other threads stealing, the answer would
// be stale as soon as it returned. An empty optional from pop() or steal() is
// the only emptiness signal.
//
// Not yet synchronized: the per-deque lock arrives together with the tests
// that exercise concurrent access.
template <typename T>
class WorkStealingDeque {
public:
    // Owner only. Adds an item at the back.
    void push(T item) { items_.push_back(std::move(item)); }

    // Owner only. Takes the front item, or returns nothing if the deque is empty.
    std::optional<T> pop() {
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> item{std::move(items_.front())};
        items_.pop_front();
        return item;
    }

    // Any other thread. Takes the back item, or returns nothing if the deque is empty.
    std::optional<T> steal() {
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> item{std::move(items_.back())};
        items_.pop_back();
        return item;
    }

private:
    std::deque<T> items_;
};

}  // namespace tsched
