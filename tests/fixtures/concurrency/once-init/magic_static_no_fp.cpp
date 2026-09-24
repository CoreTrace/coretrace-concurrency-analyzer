// SPDX-License-Identifier: Apache-2.0
// A function-local static is initialized once, by whichever thread gets there first, and every
// other thread waits for that initialization to finish: the constructor never runs twice and
// every read of the object comes after it.
#include <atomic>
#include <thread>
#include <vector>

struct Service
{
    int id;
    Service() : id(7) {}
};

static int service_id()
{
    static Service instance;
    return instance.id;
}

static std::atomic<int> total{0};

static void reader()
{
    total += service_id();
}

int main()
{
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++)
        threads.emplace_back(reader);
    for (std::thread& thread : threads)
        thread.join();
    return total == 28 ? 0 : 1;
}
