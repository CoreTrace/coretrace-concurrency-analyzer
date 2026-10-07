// SPDX-License-Identifier: Apache-2.0
// A function returns the vector it filled with the threads it started; main's loop joins every
// thread of it before writing: nothing runs beside that write.
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
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
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
