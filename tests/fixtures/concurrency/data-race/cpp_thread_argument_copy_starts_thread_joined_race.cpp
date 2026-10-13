// SPDX-License-Identifier: Apache-2.0
// Copying the argument handed to the std::thread constructor starts a detached thread of its own.
// Joining the std::thread ends its worker, not that thread, which still runs when main writes
// (#200).
// Expected: two data races, main against background and worker against background; main does
// not race with worker, joined before the write.
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

static void worker(Argument)
{
    shared += 1;
}

int main()
{
    Argument argument;
    std::thread thread(worker, argument);
    thread.join();
    shared += 1;
    return shared;
}
