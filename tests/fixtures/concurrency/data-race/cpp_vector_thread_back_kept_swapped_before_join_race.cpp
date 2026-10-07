// SPDX-License-Identifier: Apache-2.0
// The element back() reads is kept, swapped for another thread, then joined: the join ends the
// other thread, and the one moved into the vector still runs when main writes.
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
    threads.push_back(std::thread(reader));
    std::thread other([] {});
    std::thread& last = threads.back();
    last.swap(other);
    last.join();
    shared = 1;
    other.join();
    return shared;
}
