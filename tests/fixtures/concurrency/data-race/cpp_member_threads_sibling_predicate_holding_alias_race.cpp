// SPDX-License-Identifier: Apache-2.0
// The helper emptying the member vector filters a sibling member with a predicate holding a
// reference that aliases that vector: the predicate detaches the reader and erases it, so the loop
// ends without joining it, and it still runs when main writes.
// Expected: one data race, main against reader.
#include <list>
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
    std::list<int> others;
};

static std::vector<std::thread>& identity(std::vector<std::thread>& threads)
{
    return threads;
}

struct DetachFirst
{
    std::vector<std::thread>& threads;

    bool operator()(int) const
    {
        if (!threads.empty())
        {
            threads.front().detach();
            threads.erase(threads.begin());
        }
        return true;
    }
};

static void drain(Pool& pool, std::vector<std::thread>& alias)
{
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pool.others.remove_if(DetachFirst{alias});
    }
}

int main()
{
    Pool pool;
    pool.others.push_back(1);
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread([] {}));
    drain(pool, identity(pool.threads));
    shared = 1;
    return shared;
}
