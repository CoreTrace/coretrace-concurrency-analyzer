// SPDX-License-Identifier: Apache-2.0
// The loop pops the joined threads of the vector while another one, holding a single thread, is
// not empty: it runs once, and the vector's first thread, a reader, still runs when main writes.
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
    std::vector<std::thread> others;
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread([] {}));
    others.emplace_back(std::thread([] {}));
    while (!others.empty())
    {
        threads.back().join();
        threads.pop_back();
        others.back().join();
        others.pop_back();
    }
    shared = 1;
    threads.back().join();
    return shared;
}
