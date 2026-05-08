// SPDX-License-Identifier: Apache-2.0
// Test: moved std::thread joined through the destination handle should not produce MissingJoin.
#include <thread>
#include <utility>

void worker()
{
}

int main()
{
    std::thread first(worker);
    std::thread second(std::move(first));
    second.join();
    return 0;
}
