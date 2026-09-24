// SPDX-License-Identifier: Apache-2.0
// The release store comes before the payload is written, so it publishes nothing: the consumer
// can observe the flag while the producer is still writing.
#include <atomic>
#include <thread>

static int payload = 0;
static std::atomic<bool> ready{false};
static int observed = 0;

static void producer()
{
    ready.store(true, std::memory_order_release);
    payload = 42;
}

static void consumer()
{
    while (!ready.load(std::memory_order_acquire))
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
