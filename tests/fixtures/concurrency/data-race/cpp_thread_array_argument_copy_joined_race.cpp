// SPDX-License-Identifier: Apache-2.0
// Each thread moved into the array is built from a temporary whose argument copy starts a detached
// thread. The loop joining the array ends the workers, not those threads, which still run when
// main writes (#200).
// Expected: four data races, main against background, worker against background, background
// against background and worker against worker; main does not race with worker.
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
    std::thread threads[2];
    for (int i = 0; i < 2; ++i)
        threads[i] = std::thread(worker, argument);
    for (int i = 0; i < 2; ++i)
        threads[i].join();
    shared += 1;
    return shared;
}
