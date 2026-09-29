// SPDX-License-Identifier: Apache-2.0
// std::lock takes both mutexes without deadlocking, whatever order it is given them. Each thread
// then hands them to guards built with std::adopt_lock, in opposite orders. An adopting guard takes
// over a lock its thread already holds and waits for nothing, so those orders cannot deadlock.
// Expected: no diagnostic.
#include <mutex>
#include <thread>

static std::mutex first;
static std::mutex second;
static int shared;

static void forward()
{
    std::lock(first, second);
    std::lock_guard<std::mutex> a(first, std::adopt_lock);
    std::lock_guard<std::mutex> b(second, std::adopt_lock);
    ++shared;
}

int main()
{
    std::thread worker(forward);
    {
        std::lock(second, first);
        std::lock_guard<std::mutex> b(second, std::adopt_lock);
        std::lock_guard<std::mutex> a(first, std::adopt_lock);
        ++shared;
    }
    worker.join();
    return shared;
}
