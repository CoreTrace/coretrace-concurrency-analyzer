// SPDX-License-Identifier: Apache-2.0
// The helper takes the std::thread by reference but detaches it: the lambda still reads the
// local after `sum_locally` has returned.
#include <thread>

static int total = 0;

static void let_go(std::thread& worker)
{
    worker.detach();
}

static void sum_locally()
{
    int local_value = 42;
    std::thread worker([&local_value] { total += local_value; });
    let_go(worker);
}

int main()
{
    sum_locally();
    return total;
}
