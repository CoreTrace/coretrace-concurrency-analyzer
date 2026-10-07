// SPDX-License-Identifier: Apache-2.0
// A helper moves the threads it starts into the vector it is handed; main's loop joins every
// thread of the vector before writing: nothing runs beside that write.
// Expected: no diagnostic.
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
    add(threads);
    add(threads);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
