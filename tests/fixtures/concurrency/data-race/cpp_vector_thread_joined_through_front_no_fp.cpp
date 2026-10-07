// SPDX-License-Identifier: Apache-2.0
// The only thread moved into the vector is joined through front() before main writes: nothing
// runs beside that write.
// Expected: no diagnostic.
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
    threads.front().join();
    shared = 1;
    return shared;
}
