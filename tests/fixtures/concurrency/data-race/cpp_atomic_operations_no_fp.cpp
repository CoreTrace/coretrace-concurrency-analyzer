// SPDX-License-Identifier: Apache-2.0
// Regression: atomic operations are ordered by the memory model and never race with each
// other. Their lowering to `load atomic`/`store atomic` and to libc++ wrappers was treated
// as plain shared access.
#include <atomic>
#include <thread>

std::atomic<int> atomic_counter{0};

void add_one()
{
    atomic_counter.fetch_add(1);
}

void add_and_read()
{
    atomic_counter.fetch_add(1);
    const int observed = atomic_counter.load();
    (void)observed;
}

int main()
{
    std::thread first(add_one);
    std::thread second(add_and_read);
    first.join();
    second.join();
    return atomic_counter.load();
}
