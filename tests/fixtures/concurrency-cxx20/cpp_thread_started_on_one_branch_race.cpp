// SPDX-License-Identifier: Apache-2.0
// The thread starts on one branch, and joinable() guards its join: main's write between them
// races, and its read after the join does not (#113).
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
    if (argc > 1)
        thread = std::thread(worker);
    int local = 0;
    shared += 1;
    if (thread.joinable())
        thread.join();
    return shared + local;
}
