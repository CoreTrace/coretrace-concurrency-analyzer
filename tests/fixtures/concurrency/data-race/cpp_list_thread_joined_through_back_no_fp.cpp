// SPDX-License-Identifier: Apache-2.0
// The only thread moved into the list is joined through back() before main writes: nothing runs
// beside that write.
// Expected: no diagnostic.
#include <list>
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::list<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    threads.back().join();
    shared = 1;
    return shared;
}
