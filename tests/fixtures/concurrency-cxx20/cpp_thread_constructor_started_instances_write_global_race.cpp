// SPDX-License-Identifier: Apache-2.0
// A helper builds each Worker in place, in a loop; each constructor starts a thread on its own
// object, and every thread adds to the same global. The threads run together: each holds its own
// object, but not its own global (#126).
// Expected: one data race, run against run.
#include <new>
#include <thread>

static int total;

struct Worker
{
    explicit Worker(int value) : n(value), thread(&Worker::run, this) {}
    void run()
    {
        total += n;
    }
    int n;
    std::thread thread;
};

static void build(Worker* place, int value)
{
    ::new (place) Worker(value);
}

int main()
{
    Worker* workers[3];
    for (int i = 0; i < 3; ++i)
    {
        workers[i] = static_cast<Worker*>(::operator new(sizeof(Worker)));
        build(workers[i], i);
    }
    for (int i = 0; i < 3; ++i)
    {
        workers[i]->thread.join();
        workers[i]->~Worker();
        ::operator delete(workers[i]);
    }
    return 0;
}
