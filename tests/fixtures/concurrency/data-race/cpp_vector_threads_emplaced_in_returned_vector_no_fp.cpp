// SPDX-License-Identifier: Apache-2.0
// A function returns a vector it built its threads in with emplace_back; main joins every element
// before writing (#123, form s06).
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static std::vector<std::thread> make()
{
    std::vector<std::thread> threads;
    threads.emplace_back(reader);
    threads.emplace_back(reader);
    return threads;
}

int main()
{
    std::vector<std::thread> threads = make();
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
