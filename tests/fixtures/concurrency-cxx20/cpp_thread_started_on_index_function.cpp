// SPDX-License-Identifier: Apache-2.0
// A thread starts on bump() with index 0, and main calls bump(0) too: both increment slots[0].
// The thread's run of bump() comes from no call the analysis sees, so its element stays unknown,
// and the race stays reported (#159).
// Expected: one data race on `slots`, main's call to bump() against the thread's run.
#include <thread>

static int slots[2];

static void bump(int index)
{
    slots[index] += 1;
}

int main()
{
    std::thread worker(bump, 0);
    bump(0);
    worker.join();
    return slots[0];
}
