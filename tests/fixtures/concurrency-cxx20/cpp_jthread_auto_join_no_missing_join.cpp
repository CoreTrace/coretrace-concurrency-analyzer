// SPDX-License-Identifier: Apache-2.0
// Regression: std::jthread joins in its destructor. Its mangled constructor contains
// "threadC1", so it was classified as a plain std::thread whose join never came back.
//
// This fixture lives outside tests/fixtures/concurrency/ because the CLI fixture suite
// compiles that tree with the default standard, and std::jthread requires C++20.
#include <thread>

static void worker() {}

int main()
{
    std::jthread handle(worker);
    return 0;
}
