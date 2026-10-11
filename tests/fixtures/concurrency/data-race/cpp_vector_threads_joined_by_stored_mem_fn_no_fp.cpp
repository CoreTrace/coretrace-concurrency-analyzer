// SPDX-License-Identifier: Apache-2.0
// The std::mem_fn(&std::thread::join) object is kept in a variable, then std::for_each applies it
// to every element of the vector before main writes: nothing runs beside that write.
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
    const auto joiner = std::mem_fn(&std::thread::join);
    std::for_each(threads.begin(), threads.end(), joiner);
    shared = 1;
    return shared;
}
