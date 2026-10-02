// SPDX-License-Identifier: Apache-2.0
// The index loop stops below a helper's count read from the vector, half its size: it joins the
// idle thread only, and the writer may still run when main writes after the loop (#162).
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

static std::size_t half(const std::vector<std::thread>& threads) noexcept
{
    return threads.size() / 2;
}

int main()
{
    std::vector<std::thread> threads;
    threads.push_back(std::thread(idle));
    threads.push_back(std::thread(writer));
    for (std::size_t i = 0; i < half(threads); ++i)
        threads[i].join();
    shared += 2;
    threads[1].join();
    return shared;
}
