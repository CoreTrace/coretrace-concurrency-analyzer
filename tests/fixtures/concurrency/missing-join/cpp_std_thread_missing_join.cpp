// SPDX-License-Identifier: Apache-2.0
// Test: Missing join C++ - std::thread joinable a la sortie du scope
#include <thread>

void worker()
{
}

int main()
{
    std::thread thread(worker);
    return 0;
}
