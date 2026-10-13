// SPDX-License-Identifier: Apache-2.0
// The vector has an allocator of its own, which starts no thread: joining back() still ends the
// one thread emplace_back built, before main writes (#123).
// Expected: no data race.
#include <cstddef>
#include <memory>
#include <thread>
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
