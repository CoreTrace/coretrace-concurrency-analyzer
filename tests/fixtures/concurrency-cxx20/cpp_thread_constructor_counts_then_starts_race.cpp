// SPDX-License-Identifier: Apache-2.0
// Built in a loop, each Worker adds to total in its constructor, then starts a thread that adds
// to total too; the threads run until main ends. Each thread races with the next one, and with
// the next constructor's count() (#126).
// Expected: two data races on total: run against run, and run against count.
#include <memory>
#include <thread>
#include <vector>

static int total;

static int count(int* sum)
{
    *sum += 1;
    return *sum;
}

struct Worker
{
    explicit Worker(int* sum) : seen(count(sum)), thread(&Worker::run, this) {}
    ~Worker()
    {
        thread.join();
    }
    void run()
    {
        total += seen;
    }
    int seen;
    std::thread thread;
};

int main()
{
    std::vector<std::shared_ptr<Worker>> workers;
    for (int i = 0; i < 3; ++i)
        workers.push_back(std::shared_ptr<Worker>(new Worker(&total)));
    return 0;
}
