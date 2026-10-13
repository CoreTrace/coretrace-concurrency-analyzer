// SPDX-License-Identifier: Apache-2.0
// The std::thread runs a lambda, whose argument's copy starts a detached thread of its own.
// Joining the std::thread ends the lambda's thread, not that one, which still runs when main
// writes (#200). The lambda accesses nothing: what a lambda's thread accesses is not followed on
// every standard library (#125).
// Expected: one data race, main against background.
#include <pthread.h>
#include <thread>

static int shared;

static void* background(void*)
{
    shared += 1;
    return nullptr;
}

struct Argument
{
    Argument() = default;
    Argument(const Argument&)
    {
        pthread_t extra;
        pthread_create(&extra, nullptr, background, nullptr);
        pthread_detach(extra);
    }
};

int main()
{
    Argument argument;
    std::thread thread([](Argument) {}, argument);
    thread.join();
    shared += 1;
    return shared;
}
