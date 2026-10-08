// SPDX-License-Identifier: Apache-2.0
// An idle thread pushed to the deque's front after the reader is the one front() joins: the
// reader still runs when main writes.
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
    threads.emplace_back(std::thread(reader));
    threads.emplace_front(std::thread([] {}));
    threads.front().join();
    shared = 1;
    threads.back().join();
    return shared;
}
