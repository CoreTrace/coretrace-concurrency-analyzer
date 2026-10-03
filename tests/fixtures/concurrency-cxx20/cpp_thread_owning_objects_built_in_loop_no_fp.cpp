// SPDX-License-Identifier: Apache-2.0
// Each Worker starts its thread on itself in its constructor, after setting n; make_shared builds
// one per round. The thread of a round still runs while the next Worker is constructed, but it
// holds the previous object (#113).
// Expected: no diagnostic.
#include <memory>
#include <thread>
#include <vector>

struct Worker
{
    explicit Worker(int value) : n(value), thread(&Worker::run, this) {}
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
    std::vector<std::shared_ptr<Worker>> workers;
    for (int i = 0; i < 3; ++i)
        workers.push_back(std::make_shared<Worker>(i));
    return 0;
}
