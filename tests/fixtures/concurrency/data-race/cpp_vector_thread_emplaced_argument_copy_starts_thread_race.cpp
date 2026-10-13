// SPDX-License-Identifier: Apache-2.0
// Copying the argument emplace_back hands the new thread starts a detached thread of its own.
// Joining the element ends the thread emplace_back built, not that one, which still runs when
// main writes (#123).
// Expected: one data race, main against background.
#include <pthread.h>
#include <thread>
#include <vector>

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

static void worker(Argument) {}

int main()
{
    std::vector<std::thread> threads;
    Argument argument;
    threads.emplace_back(worker, argument);
    threads.back().join();
    shared += 1;
    return shared;
}
