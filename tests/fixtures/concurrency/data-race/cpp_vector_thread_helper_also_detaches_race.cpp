// SPDX-License-Identifier: Apache-2.0
// The helper moves one thread into the vector and detaches another: main's loop joins the first,
// and the detached one still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void add(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread([] {}));
    std::thread(reader).detach();
}

int main()
{
    std::vector<std::thread> threads;
    add(threads);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
