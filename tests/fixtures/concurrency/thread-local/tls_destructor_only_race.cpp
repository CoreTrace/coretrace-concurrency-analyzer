// SPDX-License-Identifier: Apache-2.0
// Each thread's thread-local object is destroyed when that thread ends, in that thread: the four
// destructors increment the same plain counter concurrently.
#include <thread>
#include <vector>

static int finished = 0;

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
