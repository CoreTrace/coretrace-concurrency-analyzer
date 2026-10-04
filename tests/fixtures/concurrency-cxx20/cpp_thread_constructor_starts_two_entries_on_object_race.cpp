// SPDX-License-Identifier: Apache-2.0
// The constructor starts two threads, of two entries, on the object it builds, and both write n.
// Each entry's instances hold objects of their own, but the two entries share this one (#126).
// Expected: one data race, run against log.
#include <thread>

struct Worker
{
    Worker() : first(&Worker::run, this), second(&Worker::log, this) {}
    ~Worker()
    {
        first.join();
        second.join();
    }
    void run()
    {
        n += 1;
    }
    void log()
    {
        n += 2;
    }
    int n = 0;
    std::thread first;
    std::thread second;
};

int main()
{
    Worker worker;
    return 0;
}
