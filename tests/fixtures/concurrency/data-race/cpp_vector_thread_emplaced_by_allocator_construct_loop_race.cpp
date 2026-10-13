// SPDX-License-Identifier: Apache-2.0
// The allocator's construct builds the element twice in a loop and detaches the first thread: the
// one construction emplace_back reaches runs twice, so joining the element leaves the detached
// thread running when main writes, beside the element's own (#123).
// Expected: two data races, main against worker and worker against worker.
#include <cstddef>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

template <class T> struct Alloc
{
    using value_type = T;
    Alloc() = default;
    template <class U> Alloc(const Alloc<U>&) {}
    T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) { std::allocator<T>{}.deallocate(p, n); }
    template <class U, class... Args> void construct(U* p, Args&&... args)
    {
        // A thread relocated by the vector is moved once, as usual.
        if constexpr ((std::is_same_v<std::decay_t<Args>, U> || ...))
            ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
        else
            for (int round = 0; round < 2; ++round)
            {
                ::new (static_cast<void*>(p)) U(args...);
                if (round == 0)
                    p->detach();
            }
    }
    template <class U> bool operator==(const Alloc<U>&) const { return true; }
};

int main()
{
    std::vector<std::thread, Alloc<std::thread>> threads;
    threads.emplace_back(worker);
    threads.back().join();
    shared += 1;
    return shared;
}
