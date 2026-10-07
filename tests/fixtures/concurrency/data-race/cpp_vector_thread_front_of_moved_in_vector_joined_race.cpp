// SPDX-License-Identifier: Apache-2.0
// The vector is built from another already holding a thread: front() joins that thread, not the
// one moved in afterwards, which still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::vector<std::thread> idle;
    idle.push_back(std::thread([] {}));
    std::vector<std::thread> threads(std::move(idle));
    threads.push_back(std::thread(reader));
    threads.front().join();
    shared = 1;
    threads.back().join();
    return shared;
}
