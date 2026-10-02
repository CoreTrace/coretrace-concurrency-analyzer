// SPDX-License-Identifier: Apache-2.0
// Only one case of the switch starts the thread: main's write after the switch may run beside
// it, and the one after its join does not (#113).
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
    switch (argc)
    {
    case 2:
        thread = std::thread(worker);
        break;
    case 3:
        return 3;
    default:
        break;
    }
    shared += 1;
    if (argc == 2)
        thread.join();
    shared += 2;
    return shared;
}
