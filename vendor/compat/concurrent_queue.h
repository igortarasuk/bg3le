#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// MSVC ships concurrency::concurrent_queue in <concurrent_queue.h>. See
// concurrent_vector.h for why this is not mapped onto oneTBB: it would add
// libtbb.so.12, which the Steam runtime container does not have.
//
// bg3se uses push, try_pop, empty and clear. A mutex-guarded std::list gives
// all four with the same semantics -- unlike the vector, nothing here depends
// on lock-free behaviour or on references surviving, since try_pop moves the
// element out.
//
// std::list rather than std::deque because one of these is declared as
// concurrent_queue<SDL_Event> where SDL_Event is only forward-declared, and
// deque needs a complete type at class scope to compute its block size. C++17
// allows list, vector and forward_list to be instantiated with an incomplete
// type; deque is not on that list.
//

#include <list>
#include <memory>
#include <mutex>
#include <utility>

namespace concurrency {

template <class T, class TAllocator = std::allocator<T>>
class concurrent_queue
{
public:
    using value_type = T;
    using size_type = std::size_t;

    void push(T const& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(value);
    }

    void push(T&& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(value));
    }

    bool try_pop(T& out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.empty();
    }

    size_type unsafe_size() const { return items_.size(); }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
    }

private:
    std::list<T, TAllocator> items_;
    mutable std::mutex mutex_;
};

}  // namespace concurrency

namespace Concurrency = concurrency;
