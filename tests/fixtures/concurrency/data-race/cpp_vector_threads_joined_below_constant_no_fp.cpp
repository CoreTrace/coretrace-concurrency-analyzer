// SPDX-License-Identifier: Apache-2.0
// Three threads are moved into the vector, one a round of a loop counting to 3; a loop joining
// threads[i] for i below 3 joins them all before main writes: nothing runs beside that write.
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
    for (int i = 0; i < 3; ++i)
        threads.push_back(std::thread(reader));
    for (int i = 0; i < 3; ++i)
        threads[i].join();
    shared = 1;
    return shared;
}
