// SPDX-License-Identifier: Apache-2.0
// The helper is handed the same vector twice: the thread it moves in through one reference, it
// takes out again through the other before the loop, which then joins nothing. The writer still
// runs when the helper writes after the loop (#162).
// Expected: one data race, the helper's write against the writer's.
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void writer()
{
    shared += 1;
}

static void run(std::vector<std::thread>& threads, std::vector<std::thread>& same)
{
    threads.push_back(std::thread(writer));
    std::thread taken = std::move(same.back());
    same.pop_back();
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    taken.join();
}

int main()
{
    std::vector<std::thread> threads;
    run(threads, threads);
    return 0;
}
