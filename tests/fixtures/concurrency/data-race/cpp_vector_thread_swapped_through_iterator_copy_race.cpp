// SPDX-License-Identifier: Apache-2.0
// Through a copy of the iterator it is handed, the function template swaps the first thread out
// for an idle one and detaches it, then joins the range: the swapped-out thread still runs when
// main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

template <class Iterator> static void joinRangeSwappingFirst(Iterator first, Iterator last)
{
    Iterator held = first;
    std::thread idle([] {});
    held->swap(idle);
    idle.detach();
    for (; first != last; ++first)
        first->join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinRangeSwappingFirst(threads.begin(), threads.end());
    shared = 1;
    return shared;
}
