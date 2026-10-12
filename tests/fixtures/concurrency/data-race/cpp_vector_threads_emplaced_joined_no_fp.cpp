// SPDX-License-Identifier: Apache-2.0
// emplace_back builds each thread in the vector from the entry it is handed, and a loop joins
// every element before main writes: nothing runs beside that write (#123).
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
    for (int i = 0; i < 2; ++i)
        threads.emplace_back(reader);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
