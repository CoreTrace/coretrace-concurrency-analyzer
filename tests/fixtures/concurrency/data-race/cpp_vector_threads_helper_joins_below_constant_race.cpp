// SPDX-License-Identifier: Apache-2.0
// The helper joins the vector's first thread only, its loop running below a constant: the second
// thread still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinFirst(std::vector<std::thread>& threads)
{
    for (int index = 0; index < 1; ++index)
        threads[index].join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread([] {}));
    threads.emplace_back(std::thread(reader));
    joinFirst(threads);
    shared = 1;
    threads.back().join();
    return shared;
}
