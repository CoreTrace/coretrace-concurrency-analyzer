// SPDX-License-Identifier: Apache-2.0
// std::for_each applies std::mem_fn(&std::thread::join) to every element of the vector before
// main writes: nothing runs beside that write.
// Expected: no diagnostic.
#include <algorithm>
#include <functional>
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
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
    std::for_each(threads.begin(), threads.end(), std::mem_fn(&std::thread::join));
    shared = 1;
    return shared;
}
