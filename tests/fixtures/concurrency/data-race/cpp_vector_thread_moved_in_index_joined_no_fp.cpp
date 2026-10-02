// SPDX-License-Identifier: Apache-2.0
// Two threads moved into the vector are joined by an index loop over its whole size before main
// writes (#162).
// Expected: no diagnostic.
#include <cstddef>
#include <thread>
#include <vector>

static int shared;

static void writer()
{
    shared += 1;
}

static void idle() {}

int main()
{
    std::vector<std::thread> threads;
    threads.push_back(std::thread(writer));
    threads.push_back(std::thread(idle));
    for (std::size_t i = 0; i < threads.size(); ++i)
        threads[i].join();
    shared += 2;
    return shared;
}
