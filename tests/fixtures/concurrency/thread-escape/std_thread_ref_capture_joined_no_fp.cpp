// SPDX-License-Identifier: Apache-2.0
// The lambda captures a local by reference, but the thread is joined before the function
// returns, so the frame outlives the thread.
#include <thread>
#include <vector>

static int total = 0;

static void sum_locally()
{
    std::vector<int> values = {1, 2, 3};
    std::thread worker(
        [&values]
        {
            for (int value : values)
                total += value;
        });
    worker.join();
}

int main()
{
    sum_locally();
    return total;
}
