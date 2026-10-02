// SPDX-License-Identifier: Apache-2.0
// Each round swaps the element with a spare thread before joining it: the loop joins the idle
// thread, and the writer, now the spare, still runs when main writes after the loop (#162).
// Expected: one data race, main's write against the writer's.
#include <thread>
#include <vector>

static int shared;

static void writer()
{
    shared += 1;
}

static void idle() {}

int main()
{
    std::vector<std::thread> threads;
    threads.push_back(std::thread(writer));
    std::thread spare(idle);
    for (auto& thread : threads)
    {
        thread.swap(spare);
        thread.join();
    }
    shared += 2;
    spare.join();
    return shared;
}
