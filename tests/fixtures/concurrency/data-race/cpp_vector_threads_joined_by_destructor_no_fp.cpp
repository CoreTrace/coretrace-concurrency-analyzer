// SPDX-License-Identifier: Apache-2.0
// The destructor of the object holding the vector joins every thread of it when its scope ends,
// before main writes: nothing runs beside that write.
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

struct Pool
{
    std::vector<std::thread> threads;

    ~Pool()
    {
        for (auto& thread : threads)
            thread.join();
    }
};

int main()
{
    {
        Pool pool;
        pool.threads.emplace_back(std::thread(reader));
        pool.threads.emplace_back(std::thread(reader));
    }
    shared = 1;
    return shared;
}
