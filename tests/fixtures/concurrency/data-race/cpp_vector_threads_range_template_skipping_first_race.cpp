// SPDX-License-Identifier: Apache-2.0
// The function template steps past the first element before joining the others: the vector's
// first thread still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

template <class Iterator> static void joinAllButFirst(Iterator first, Iterator last)
{
    ++first;
    for (; first != last; ++first)
        first->join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread([] {}));
    joinAllButFirst(threads.begin(), threads.end());
    shared = 1;
    threads.front().join();
    return shared;
}
