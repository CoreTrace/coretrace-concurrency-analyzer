// SPDX-License-Identifier: Apache-2.0
// The same with a global object: the thread's accesses through `this` are accesses to the global
// the owner writes by name.
#include <thread>

struct Counter
{
    int value = 0;

    void run()
    {
        value += 1;
    }
};

Counter counter;

int main()
{
    std::thread worker(&Counter::run, &counter);
    counter.value += 1;
    worker.join();
    return counter.value;
}
