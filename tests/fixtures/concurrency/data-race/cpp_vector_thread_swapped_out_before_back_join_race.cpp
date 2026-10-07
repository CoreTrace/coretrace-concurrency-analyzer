// SPDX-License-Identifier: Apache-2.0
// The thread moved into the vector is swapped out of back() for another before back() is joined:
// the join ends the other thread, and the first still runs when main writes.
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
    threads.back().swap(other);
    threads.back().join();
    shared = 1;
    other.join();
    return shared;
}
