// SPDX-License-Identifier: Apache-2.0
// The thread-local destructors run concurrently at the end of their threads, but the counter
// they update is atomic.
#include <atomic>
#include <thread>
#include <vector>

static std::atomic<int> finished{0};

struct Tracker
{
    ~Tracker()
    {
        finished++;
    }
};

static void worker()
{
    thread_local Tracker tracker;
    (void)tracker;
}

int main()
{
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++)
        threads.emplace_back(worker);
    for (std::thread& thread : threads)
        thread.join();
    return finished;
}
