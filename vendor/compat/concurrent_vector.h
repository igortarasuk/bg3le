#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// MSVC ships concurrency::concurrent_vector in <concurrent_vector.h>.
//
// This was originally mapped onto oneTBB, which built and linked but added
// libtbb.so.12 as a runtime dependency -- and that is not in the Steam runtime
// container, so the game refused to start:
//
//   ./bin/bg3: error while loading shared libraries: libtbb.so.12
//
// The container carries TBB 2 rather than oneTBB 12, and there is no static
// libtbb to link against, so the dependency had to go rather than be
// satisfied. bg3se uses four members of this container, so a mutex-guarded
// std::vector covers it.
//
// This is weaker than the real thing: TBB keeps element references stable
// across growth and allows traversal concurrent with appends. The one caller
// here indexes rather than holding references, and uses the iterator push_back
// returns immediately, to compute an index by subtracting begin() -- so
// reallocation is not observable. If that usage changes, this has to change
// with it.
//

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace concurrency {

template <class T, class TAllocator = std::allocator<T>>
class concurrent_vector
{
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = typename std::vector<T, TAllocator>::iterator;
    using const_iterator = typename std::vector<T, TAllocator>::const_iterator;

    // Returns an iterator to the appended element, as TBB's does.
    iterator push_back(T const& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(value);
        return items_.end() - 1;
    }

    iterator push_back(T&& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(value));
        return items_.end() - 1;
    }

    void reserve(size_type n)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.reserve(n);
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
    }

    T& operator [] (size_type i) { return items_[i]; }
    T const& operator [] (size_type i) const { return items_[i]; }

    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }

    size_type size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }

private:
    std::vector<T, TAllocator> items_;
    std::mutex mutex_;
};

}  // namespace concurrency

namespace Concurrency = concurrency;
