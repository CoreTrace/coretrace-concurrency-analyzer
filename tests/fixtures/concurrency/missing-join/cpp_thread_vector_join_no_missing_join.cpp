// SPDX-License-Identifier: Apache-2.0
// Regression: the handles are moved into a container and joined through it. The move target
// is storage the analysis cannot name, so the handles must not be reported as leaked.
#include <thread>
#include <vector>

static void worker() {}

int main()
{
    std::vector<std::thread> workers;
    for (int index = 0; index < 2; ++index)
        workers.push_back(std::thread(worker));

    for (auto& handle : workers)
        handle.join();

    return 0;
}
