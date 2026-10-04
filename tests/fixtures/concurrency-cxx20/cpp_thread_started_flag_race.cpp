// SPDX-License-Identifier: Apache-2.0
// A flag set with the start guards the join: main's write before the join races, the one after
// does not (#113).
// Expected: one data race.
#include <thread>
static int shared;
static void worker()
{
    shared += 1;
}
int main(int argc, char**)
{
    std::thread thread;
    bool started = false;
    if (argc > 1)
    {
        thread = std::thread(worker);
        started = true;
    }
    shared += 1;
    if (started)
        thread.join();
    shared += 2;
    return shared;
}
