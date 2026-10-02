// SPDX-License-Identifier: Apache-2.0
// Each phase moves its thread into its own vector and joins it before the next phase starts: the
// two workers never run together, and main's read follows both joins (#162).
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void first()
{
    shared += 1;
}

static void second()
{
    shared += 2;
}

int main()
{
    std::vector<std::thread> early;
    early.push_back(std::thread(first));
    for (auto& thread : early)
        thread.join();
    std::vector<std::thread> late;
    late.push_back(std::thread(second));
    for (auto& thread : late)
        thread.join();
    return shared;
}
