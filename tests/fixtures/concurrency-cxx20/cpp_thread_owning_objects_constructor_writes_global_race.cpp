// SPDX-License-Identifier: Apache-2.0
// Each Worker's constructor writes a global before starting its thread on this; make_shared
// builds one per round. The threads of earlier rounds read that global meanwhile: shared data
// is not the object under construction (#113).
// Expected: one data race.
#include <memory>
#include <thread>
#include <vector>

int total;

struct Worker
{
    explicit Worker(int value) : n((total = value)), thread(&Worker::run, this) {}
    ~Worker()
    {
        thread.join();
    }
    void run()
    {
        int seen = total;
        (void)seen;
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
