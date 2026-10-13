// SPDX-License-Identifier: Apache-2.0
// The vector's allocator builds a second std::thread, detached, while emplace_back makes room for
// the element: emplace_back may run two constructions, so joining the element does not end the
// allocator's thread, which still runs when main writes (#123).
// Expected: one data race, main against background.
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

static int shared;

static void background()
{
    shared += 1;
}

static void worker() {}

template <class T> struct Alloc
{
    using value_type = T;
    Alloc() = default;
    template <class U> Alloc(const Alloc<U>&) {}
    T* allocate(std::size_t n)
    {
        std::thread(background).detach();
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T* p, std::size_t n) { std::allocator<T>{}.deallocate(p, n); }
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
