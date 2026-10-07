// SPDX-License-Identifier: Apache-2.0
// main moves one idle thread into the vector in a counted loop and a helper moves a reader in: the
// loop joining below the count, one, leaves the reader running when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void add(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread(reader));
}

int main()
{
    std::vector<std::thread> threads;
    for (int index = 0; index < 1; ++index)
        threads.emplace_back(std::thread([] {}));
    add(threads);
    for (int index = 0; index < 1; ++index)
        threads[index].join();
    shared = 1;
    threads.back().join();
    return shared;
}
