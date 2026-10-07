// SPDX-License-Identifier: Apache-2.0
// The helper moves an idle thread into the vector and calls a function that starts a detached
// reader: the loop joins the idle thread, and the reader still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void startDetached()
{
    std::thread(reader).detach();
}

static void add(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread([] {}));
    startDetached();
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
