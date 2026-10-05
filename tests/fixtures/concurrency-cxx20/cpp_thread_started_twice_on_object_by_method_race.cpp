// SPDX-License-Identifier: Apache-2.0
// start() is a method, not a constructor: its second call writes n on the same object while the
// thread of the first call increments it, and the two threads increment it together (#113).
// Expected: two data races: the second start() against run(), and run() against run() (#126).
#include <thread>

struct Worker
{
    void start(std::thread& thread)
    {
        n = 0;
        thread = std::thread(&Worker::run, this);
    }
    void run()
    {
        n += 1;
    }
    int n = 0;
};

int main()
{
    Worker worker;
    std::thread first;
    std::thread second;
    worker.start(first);
    worker.start(second);
    first.join();
    second.join();
    return 0;
}
