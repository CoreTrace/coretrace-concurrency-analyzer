// SPDX-License-Identifier: Apache-2.0
// The helper moves its thread into the vector on one path and detaches it on the other: past the
// loop joining the vector, the thread may still run when main writes; the program then exits.
// Expected: one data race, main against reader.
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void add(std::vector<std::thread>& threads, bool keep)
{
    std::thread thread(reader);
    if (keep)
        threads.push_back(std::move(thread));
    else
        thread.detach();
}

int main(int argc, char**)
{
    std::vector<std::thread> threads;
    add(threads, argc > 1);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    std::exit(0);
}
