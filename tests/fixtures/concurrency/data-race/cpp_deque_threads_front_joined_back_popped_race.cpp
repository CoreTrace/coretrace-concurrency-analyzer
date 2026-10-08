// SPDX-License-Identifier: Apache-2.0
// Each round joins the deque's first thread but pops the last, moved out and detached first: the
// reader at the back is never joined and still runs when main writes.
// Expected: one data race, main against reader.
#include <deque>
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::deque<std::thread> threads;
    threads.emplace_back(std::thread([] {}));
    threads.emplace_back(std::thread(reader));
    while (!threads.empty())
    {
        threads.front().join();
        threads.back().detach();
        threads.pop_back();
        if (!threads.empty())
            threads.pop_front();
    }
    shared = 1;
    return shared;
}
