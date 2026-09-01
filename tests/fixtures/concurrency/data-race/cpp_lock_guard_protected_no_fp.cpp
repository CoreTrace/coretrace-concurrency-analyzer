// SPDX-License-Identifier: Apache-2.0
// Regression: std::lock_guard is the idiomatic C++ critical section. Its constructor was
// not recognized as an acquisition, so the guarded write looked unsynchronized and the
// mutex object itself was reported as racing data.
#include <mutex>
#include <thread>

std::mutex guard;
int guarded_counter = 0;

void increment()
{
    std::lock_guard<std::mutex> held(guard);
    guarded_counter++;
}

int main()
{
    std::thread first(increment);
    std::thread second(increment);
    first.join();
    second.join();
    return guarded_counter;
}
