// SPDX-License-Identifier: Apache-2.0
// The helper emptying the member vector first stores an alias of that vector in an element of a
// sibling member, then clears the sibling each round: the element's destructor detaches the reader
// and erases it, so the loop ends without joining it, and it still runs when main writes.
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

struct DetachFirst
{
    std::vector<std::thread>* threads;

    explicit DetachFirst(std::vector<std::thread>* threads) : threads(threads) {}

    ~DetachFirst()
    {
        if (!threads->empty())
        {
            threads->front().detach();
            threads->erase(threads->begin());
        }
    }
};

struct Pool
{
    std::vector<std::thread> threads;
    std::list<DetachFirst> others;
};

static std::vector<std::thread>& identity(std::vector<std::thread>& threads)
{
    return threads;
}

static void drain(Pool& pool, std::vector<std::thread>& alias)
{
    pool.others.emplace_back(&alias);
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pool.others.clear();
    }
}

int main()
{
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread([] {}));
    drain(pool, identity(pool.threads));
    shared = 1;
    return shared;
}
