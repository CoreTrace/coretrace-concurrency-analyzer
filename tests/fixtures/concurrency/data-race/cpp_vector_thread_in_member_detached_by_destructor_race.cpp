// SPDX-License-Identifier: Apache-2.0
// The destructor joins the threads of one member vector and detaches those of the other: the
// thread moved into the detached one still runs when main writes.
// Expected: one data race, main against reader.
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
    std::vector<std::thread> detached;
    std::vector<std::thread> joined;

    ~Pool()
    {
        for (auto& thread : joined)
            thread.join();
        for (auto& thread : detached)
            thread.detach();
    }
};

int main()
{
    {
        Pool pool;
        pool.detached.emplace_back(std::thread(reader));
        pool.joined.emplace_back(std::thread([] {}));
    }
    shared = 1;
    return shared;
}
