// SPDX-License-Identifier: Apache-2.0
// A helper builds each Worker in place; its constructor writes n, then starts the thread on
// this. The threads of earlier rounds still run, but each holds the object built for it (#113).
// Expected: no diagnostic.
#include <new>
#include <thread>

struct Worker
{
    explicit Worker(int value) : n(value), thread(&Worker::run, this) {}
    void run()
    {
        n += 1;
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
