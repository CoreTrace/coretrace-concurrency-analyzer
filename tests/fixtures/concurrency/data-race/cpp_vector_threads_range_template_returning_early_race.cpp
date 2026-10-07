// SPDX-License-Identifier: Apache-2.0
// The function template returns without joining when told to: the thread may still run when main
// writes; the program then exits.
// Expected: one data race, main against reader.
#include <cstdlib>
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

template <class Iterator> static void joinRangeUnless(Iterator first, Iterator last, bool skip)
{
    if (skip)
        return;
    for (; first != last; ++first)
        first->join();
}

int main(int argc, char**)
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinRangeUnless(threads.begin(), threads.end(), argc > 1);
    shared = 1;
    std::exit(0);
}
