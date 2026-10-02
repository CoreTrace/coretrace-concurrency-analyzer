// SPDX-License-Identifier: Apache-2.0
// The helper handing the local back to push_back first swaps its thread with a spare one: the loop
// joins the idle thread, and the writer, now the spare, still runs when main writes after the
// loop (#162).
// Expected: one data race, main's write against the writer's.
#include <thread>
#include <utility>
#include <vector>

static int shared;
static std::thread spare;

static void writer()
{
    shared += 1;
}

static void idle() {}

static std::thread& exchanged(std::thread& thread)
{
    thread.swap(spare);
    return thread;
}

int main()
{
    spare = std::thread(idle);
    std::vector<std::thread> threads;
    std::thread thread(writer);
    threads.push_back(std::move(exchanged(thread)));
    for (auto& joined : threads)
        joined.join();
    shared += 2;
    spare.join();
    return shared;
}
