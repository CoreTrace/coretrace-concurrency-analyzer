// SPDX-License-Identifier: Apache-2.0
// The constructor starts a thread running run() on the object it builds; main then calls run()
// itself on that object while the thread runs. The call runs in main, not in an instance of the
// entry (#126, #155).
// Expected: one data race, main's call against the thread.
#include <thread>

struct Worker
{
    Worker() : thread(&Worker::run, this) {}
    ~Worker()
    {
        thread.join();
    }
    void run()
    {
        n += 1;
    }
    int n = 0;
    std::thread thread;
};

int main()
{
    Worker worker;
    worker.run();
    return 0;
}
