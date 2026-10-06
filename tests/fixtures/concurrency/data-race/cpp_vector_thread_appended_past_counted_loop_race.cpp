// SPDX-License-Identifier: Apache-2.0
// A loop counting to 2 moves two threads into the vector, and one more is moved in after it; the
// loop joining below 2 leaves that one running when main writes.
// Expected: one data race, main against reader.
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
        threads.push_back(std::thread(reader));
    threads.push_back(std::thread(reader));
    for (int i = 0; i < 2; ++i)
        threads[i].join();
    shared = 1;
    threads[2].join();
    return shared;
}
