// SPDX-License-Identifier: Apache-2.0
// The helper moves its thread into the vector it is handed second; main's loop joins the first:
// the thread still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void add(std::vector<std::thread>& joined, std::vector<std::thread>& filled)
{
    (void)joined;
    filled.emplace_back(std::thread(reader));
}

int main()
{
    std::vector<std::thread> threads;
    std::vector<std::thread> others;
    add(threads, others);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    others.back().join();
    return shared;
}
