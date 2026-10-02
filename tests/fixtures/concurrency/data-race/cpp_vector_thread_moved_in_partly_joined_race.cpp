// SPDX-License-Identifier: Apache-2.0
// The index loop starts at 1 and never joins the writer, at 0: the writer may still run when main
// writes after the loop (#162).
// Expected: one data race, main's write against the writer's.
#include <cstddef>
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
    threads.push_back(std::thread(idle));
    for (std::size_t i = 1; i < threads.size(); ++i)
        threads[i].join();
    shared += 2;
    threads[0].join();
    return shared;
}
