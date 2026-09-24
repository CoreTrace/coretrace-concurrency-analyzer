// SPDX-License-Identifier: Apache-2.0
// A second thread also sets the flag, without publishing anything. Observing the flag no longer
// proves that the producer's store was the one read, so the payload is not ordered.
#include <atomic>
#include <thread>

static int payload = 0;
static std::atomic<bool> ready{false};
static int observed = 0;

static void producer()
{
    payload = 42;
    ready.store(true, std::memory_order_release);
}

static void consumer()
{
    while (!ready.load(std::memory_order_acquire))
    {
    }
    observed = payload;
}

static void interloper()
{
    ready.store(true, std::memory_order_release);
}

int main()
{
    std::thread first(producer);
    std::thread second(consumer);
    std::thread third(interloper);
    first.join();
    second.join();
    third.join();
    return observed;
}
