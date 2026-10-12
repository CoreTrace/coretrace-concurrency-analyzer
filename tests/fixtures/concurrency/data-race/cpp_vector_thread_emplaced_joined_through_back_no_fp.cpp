// SPDX-License-Identifier: Apache-2.0
// The one thread emplace_back builds in the vector is the element back() reads, and it is joined
// before main writes (#123, form c01).
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
    threads.back().join();
    shared = 1;
    return shared;
}
