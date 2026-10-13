// SPDX-License-Identifier: Apache-2.0
// The vector's allocator starts a detached thread while emplace_back makes room for the element.
// Joining the element ends the thread emplace_back built, not the allocator's, which still runs
// when main writes (#123).
// Expected: one data race, main against background.
#include <cstddef>
#include <memory>
#include <pthread.h>
#include <thread>
#include <vector>

static int shared;

static void* background(void*)
{
    shared += 1;
    return nullptr;
}

static void worker() {}

template <class T> struct Alloc
{
    using value_type = T;
    Alloc() = default;
    template <class U> Alloc(const Alloc<U>&) {}
    T* allocate(std::size_t n)
    {
        pthread_t extra;
        pthread_create(&extra, nullptr, background, nullptr);
        pthread_detach(extra);
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
