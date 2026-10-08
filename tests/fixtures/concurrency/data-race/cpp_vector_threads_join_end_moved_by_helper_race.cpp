// SPDX-License-Identifier: Apache-2.0
// A helper moves the end the loop stops at back by one before the loop joins from begin(): the last
// thread, a reader, is never joined and still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

using Iterator = std::vector<std::thread>::iterator;

static void shrink(int, Iterator& last)
{
    --last;
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(idle));
    threads.emplace_back(std::thread(reader));
    auto last = threads.end();
    shrink(0, last);
    for (auto it = threads.begin(); it != last; ++it)
        it->join();
    shared = 1;
    threads[1].join();
    return shared;
}
