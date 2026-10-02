// SPDX-License-Identifier: Apache-2.0
// Two threads are move-assigned into the array's two slots, and the range-for joins both before
// main writes what each wrote (#162).
// Expected: no diagnostic.
#include <thread>

static int first;
static int second;

static void raiseFirst()
{
    first += 1;
}

static void raiseSecond()
{
    second += 1;
}

int main()
{
    std::thread threads[2];
    threads[0] = std::thread(raiseFirst);
    threads[1] = std::thread(raiseSecond);
    for (auto& thread : threads)
        thread.join();
    first += second;
    return first;
}
