// SPDX-License-Identifier: Apache-2.0
// A helper hands the vector to another that moves its thread in; main's loop joins every thread of
// the vector before writing: nothing runs beside that write.
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void addOne(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread(reader));
}

static void addTwo(std::vector<std::thread>& threads)
{
    addOne(threads);
    addOne(threads);
}

int main()
{
    std::vector<std::thread> threads;
    addTwo(threads);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
