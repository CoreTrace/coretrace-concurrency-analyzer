// SPDX-License-Identifier: Apache-2.0
// main swaps the thread out through the iterator it then hands to the function joining the range,
// and detaches it: the swapped-out thread still runs when main writes.
// Expected: one data race, main against reader.
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
    auto first = threads.begin();
    std::thread idle([] {});
    first->swap(idle);
    idle.detach();
    joinRange(first, threads.end());
    shared = 1;
    return shared;
}
