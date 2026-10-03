// SPDX-License-Identifier: Apache-2.0
// The constructor writes n after starting the thread that increments it: its own construction
// runs beside the thread it started (#113).
// Expected: one data race.
#include <thread>

struct Worker
{
    Worker() : n(0), thread(&Worker::run, this)
    {
        n = 5;
    }
    ~Worker()
    {
        thread.join();
    }
    void run()
    {
        n += 1;
    }
    int n;
    std::thread thread;
};

int main()
{
    Worker worker;
    return 0;
}
