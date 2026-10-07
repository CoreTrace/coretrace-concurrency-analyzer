// SPDX-License-Identifier: Apache-2.0
// front() joins the first thread the helper moved in, an idle one: the second still runs when main
// writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void addTwo(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread([] {}));
    threads.emplace_back(std::thread(reader));
}

int main()
{
    std::vector<std::thread> threads;
    addTwo(threads);
    threads.front().join();
    shared = 1;
    threads.back().join();
    return shared;
}
