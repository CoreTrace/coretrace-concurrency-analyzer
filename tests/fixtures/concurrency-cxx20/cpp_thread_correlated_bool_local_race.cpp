// SPDX-License-Identifier: Apache-2.0
// A bool local stored once guards both the start and the join: main's write after the join does
// not race (#113).
// Expected: one data race, on before.
#include <thread>
static int before;
static int after;
static void worker()
{
    before = 1;
    after = 1;
}
int main(int argc, char**)
{
    std::thread thread;
    const bool wanted = argc > 1;
    if (wanted)
        thread = std::thread(worker);
    before = 2;
    if (wanted)
        thread.join();
    after = 2;
    return 0;
}
