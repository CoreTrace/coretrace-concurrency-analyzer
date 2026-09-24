// SPDX-License-Identifier: Apache-2.0
// Plain assignment and conversion of a std::atomic use the sequentially consistent order, which
// releases on the store and acquires on the load: the publication is ordered and the plain
// payload does not race.
#include <atomic>
#include <thread>

static int payload = 0;
static std::atomic<bool> ready{false};
static int observed = 0;

static void producer()
{
    payload = 42;
    ready = true;
}

static void consumer()
{
    while (!ready)
    {
    }
    observed = payload;
}

int main()
{
    std::thread first(producer);
    std::thread second(consumer);
    first.join();
    second.join();
    return observed;
}
