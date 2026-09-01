// SPDX-License-Identifier: Apache-2.0
// Regression: std::scoped_lock acquires both mutexes in a deadlock-free order. Twelve lines
// of correct code used to produce 111 diagnostics, one per pair of accesses lowered inside
// the guard's internal tuple.
#include <mutex>
#include <thread>

std::mutex first_lock;
std::mutex second_lock;
int guarded_total = 0;

void add_forward()
{
    std::scoped_lock held(first_lock, second_lock);
    guarded_total++;
}

void add_reverse()
{
    std::scoped_lock held(second_lock, first_lock);
    guarded_total++;
}

int main()
{
    std::thread first(add_forward);
    std::thread second(add_reverse);
    first.join();
    second.join();
    return guarded_total;
}
