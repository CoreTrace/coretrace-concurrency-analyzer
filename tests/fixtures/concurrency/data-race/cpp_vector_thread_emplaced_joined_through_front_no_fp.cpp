// SPDX-License-Identifier: Apache-2.0
// The one thread emplace_back builds in the vector is the element front() reads, and it is joined
// before main writes (#123, form s16).
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(reader);
    threads.front().join();
    shared = 1;
    return shared;
}
