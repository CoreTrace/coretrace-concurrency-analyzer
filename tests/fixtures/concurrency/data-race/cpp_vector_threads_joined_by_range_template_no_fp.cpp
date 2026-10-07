// SPDX-License-Identifier: Apache-2.0
// A function template joins every thread of the range it is handed, the whole vector, before main
// writes: nothing runs beside that write.
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

template <class Iterator> static void joinRange(Iterator first, Iterator last)
{
    for (; first != last; ++first)
        first->join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
    joinRange(threads.begin(), threads.end());
    shared = 1;
    return shared;
}
