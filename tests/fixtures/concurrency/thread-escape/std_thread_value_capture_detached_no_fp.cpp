// SPDX-License-Identifier: Apache-2.0
// The lambda captures the local by value: the thread owns its copy, so detaching it leaves
// nothing pointing into the frame.
#include <thread>
#include <vector>

static void start_detached()
{
    std::vector<int> values = {1, 2, 3};
    std::thread worker(
        [values]
        {
            volatile int sum = 0;
            for (int value : values)
                sum += value;
        });
    worker.detach();
}

int main()
{
    start_detached();
    return 0;
}
