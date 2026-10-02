// SPDX-License-Identifier: Apache-2.0
// A container of the program's own, built on std::vector, whose push_back detaches the thread it
// is given instead of keeping it: the loop over the container joins nothing, and the worker may
// still run when main writes (#162).
// Expected: one data race, main's write against the worker's.
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

struct Detaching : std::vector<std::thread>
{
    void push_back(std::thread&& thread)
    {
        thread.detach();
    }
};

int main()
{
    Detaching threads{};
    threads.push_back(std::thread(worker));
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    return shared;
}
