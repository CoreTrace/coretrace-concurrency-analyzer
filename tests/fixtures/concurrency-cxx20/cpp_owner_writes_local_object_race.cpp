// SPDX-License-Identifier: Apache-2.0
// The owner starts a thread on a member function of a local object, then writes the field the
// thread writes. The thread reaches the object through `this`, the owner by name.
#include <thread>

struct Counter
{
    int value = 0;

    void run()
    {
        value += 1;
    }
};

int main()
{
    Counter counter;
    std::thread worker(&Counter::run, &counter);
    counter.value += 1;
    worker.join();
    return counter.value;
}
