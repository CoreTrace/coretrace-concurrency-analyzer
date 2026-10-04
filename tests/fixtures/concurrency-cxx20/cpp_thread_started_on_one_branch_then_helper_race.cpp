// SPDX-License-Identifier: Apache-2.0
// A helper called after the thread may have started writes what the thread writes (#113).
// Expected: one data race.
#include <thread>
static int shared;
static void worker()
{
    shared += 1;
}
static void helper()
{
    shared += 1;
}
int main(int argc, char**)
{
    std::thread thread;
    if (argc > 1)
        thread = std::thread(worker);
    helper();
    if (argc > 1)
        thread.join();
    return 0;
}
