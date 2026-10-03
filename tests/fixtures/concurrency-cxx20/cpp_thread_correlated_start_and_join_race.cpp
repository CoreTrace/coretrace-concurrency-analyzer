// SPDX-License-Identifier: Apache-2.0
// The thread starts and is joined under the same test of argc: main's write before the join
// races with it, and the one after does not, since no thread exists when the join is skipped
// (#113).
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
    if (argc > 1)
        thread = std::thread(worker);
    before = 2;
    if (argc > 1)
        thread.join();
    after = 2;
    return 0;
}
