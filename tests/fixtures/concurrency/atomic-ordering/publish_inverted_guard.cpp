// SPDX-License-Identifier: Apache-2.0
// The consumer reads the payload on the arm where the flag is still unset, which is exactly when
// the publication has not been observed: the branch is on the flag, yet nothing orders the read.
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
    if (!ready.load(std::memory_order_acquire))
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
