// SPDX-License-Identifier: Apache-2.0
// Until the deque is empty, each round joins its first thread and pops it from the front, before
// main writes: nothing runs beside that write.
// Expected: no diagnostic.
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
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
    while (!threads.empty())
    {
        threads.front().join();
        threads.pop_front();
    }
    shared = 1;
    return shared;
}
