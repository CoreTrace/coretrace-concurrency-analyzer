// SPDX-License-Identifier: Apache-2.0
// The program's own `vector`, outside std, hides std::vector's push_back with one that detaches
// the thread: the range-for joins nothing, and the worker may still run when main writes after the
// loop (#162).
// Expected: one data race, main's write against the worker's.
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

namespace pool
{
    struct vector : std::vector<std::thread>
    {
        void push_back(std::thread&& thread)
        {
            thread.detach();
        }
    };
} // namespace pool

int main()
{
    pool::vector threads{};
    threads.push_back(std::thread(worker));
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    return shared;
}
