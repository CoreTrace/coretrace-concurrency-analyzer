// SPDX-License-Identifier: Apache-2.0
// Each thread locks the two mutexes itself, in opposite orders, and only then hands them to guards
// built with std::adopt_lock. The guards wait for nothing, but the lock() calls before them do: the
// worker holds first while waiting for second, main holds second while waiting for first.
// Expected: one deadlock, from the lock() calls. The guards reacquire nothing.
#include <mutex>
#include <thread>

static std::mutex first;
static std::mutex second;
static int shared;

static void forward()
{
    first.lock();
    second.lock();
    std::lock_guard<std::mutex> a(first, std::adopt_lock);
    std::lock_guard<std::mutex> b(second, std::adopt_lock);
    ++shared;
}

int main()
{
    std::thread worker(forward);
    {
        second.lock();
        first.lock();
        std::unique_lock<std::mutex> b(second, std::adopt_lock);
        std::unique_lock<std::mutex> a(first, std::adopt_lock);
        ++shared;
    }
    worker.join();
    return shared;
}
