// SPDX-License-Identifier: Apache-2.0
// The flag that guards the start changes before it guards the join: the join may be skipped
// while the thread runs, so both of main's writes race with it (#113).
// Expected: two data races.
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
    bool wanted = argc > 1;
    if (wanted)
        thread = std::thread(worker);
    before = 2;
    wanted = argc > 2;
    if (wanted)
        thread.join();
    after = 2;
    if (thread.joinable())
        thread.join();
    return 0;
}
