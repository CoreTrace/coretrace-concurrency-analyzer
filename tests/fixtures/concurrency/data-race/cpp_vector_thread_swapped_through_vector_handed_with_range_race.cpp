// SPDX-License-Identifier: Apache-2.0
// The function joining the range is handed the vector too, and through it swaps the last thread
// out for an idle one and detaches it: the swapped-out thread still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

template <class Iterator>
static void joinRangeSwappingLast(Iterator first, Iterator last, std::vector<std::thread>& vector)
{
    std::thread idle([] {});
    vector.back().swap(idle);
    idle.detach();
    for (; first != last; ++first)
        first->join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinRangeSwappingLast(threads.begin(), threads.end(), threads);
    shared = 1;
    return shared;
}
