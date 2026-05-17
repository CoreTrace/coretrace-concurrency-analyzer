// SPDX-License-Identifier: Apache-2.0
// Test: joined std::thread should not produce MissingJoin.
#include <thread>

void worker()
{
}

int main()
{
    std::thread thread(worker);
    thread.join();
    return 0;
}
