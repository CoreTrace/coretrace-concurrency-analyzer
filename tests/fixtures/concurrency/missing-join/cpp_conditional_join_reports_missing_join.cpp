// SPDX-License-Identifier: Apache-2.0
// Regression: the join runs only on one branch, so the other path destroys a joinable handle
// and terminates. Create and join counts balance, which used to silence the report.
#include <thread>

static void worker() {}

int main(int argc, char** argv)
{
    (void)argv;
    std::thread handle(worker);

    if (argc > 5)
        handle.join();


}
