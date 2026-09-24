// SPDX-License-Identifier: Apache-2.0
// The reader acquires, but the flag is stored relaxed, so nothing orders the payload write before
// it: the reader may see the flag set and still read the payload's old value. The payload is
// atomic, so this is no data race, only a publication that does not publish.
#include <atomic>
#include <thread>

static std::atomic<int> payload{0};
static std::atomic<bool> ready{false};
static int observed = 0;

static void producer()
{
    payload.store(42, std::memory_order_relaxed);
    ready.store(true, std::memory_order_relaxed);
}

static void consumer()
{
    while (!ready.load(std::memory_order_acquire))
    {
    }
    observed = payload.load(std::memory_order_relaxed);
}

int main()
{
    std::thread first(producer);
    std::thread second(consumer);
    first.join();
    second.join();
    return observed;
}
