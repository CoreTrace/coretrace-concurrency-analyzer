// SPDX-License-Identifier: Apache-2.0
// A std::thread captures a local by reference and a helper joins it through a reference: the
// frame outlives the thread, as with a join in place.
#include <thread>

static int total = 0;

static void finish(std::thread& worker)
{
    worker.join();
}

static void sum_locally()
{
    int local_value = 42;
    std::thread worker([&local_value] { total += local_value; });
    finish(worker);
}

int main()
{
    sum_locally();
    return total;
}
