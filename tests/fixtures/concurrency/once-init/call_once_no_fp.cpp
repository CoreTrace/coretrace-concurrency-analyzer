// SPDX-License-Identifier: Apache-2.0
// std::call_once runs the callable exactly once and makes every caller wait for it, so the
// readers see the configuration it wrote.
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

static std::once_flag once;
static int config = 0;
static std::atomic<int> total{0};

static void reader()
{
    std::call_once(once, [] { config = 42; });
    total += config;
}

int main()
{
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++)
        threads.emplace_back(reader);
    for (std::thread& thread : threads)
        thread.join();
    return total == 168 ? 0 : 1;
}
