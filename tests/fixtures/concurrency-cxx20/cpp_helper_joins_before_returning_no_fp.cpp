// SPDX-License-Identifier: Apache-2.0
// The counterpart that keeps the rule honest: a helper that starts a thread and waits for it
// hands nothing back. Treating every spawning callee as leaving a thread behind would report a
// race in all correctly written code of this shape.
#include <thread>

int shared_total = 0;

static void accumulate()
{
    shared_total = shared_total + 1;
}

static void run_once()
{
    std::thread worker(accumulate);
    worker.join();
}

int main()
{
    run_once();
    shared_total = shared_total + 1;
    return shared_total;
}
