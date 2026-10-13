// SPDX-License-Identifier: Apache-2.0
// Each thread pushed into the vector is built from a temporary whose argument copy starts a
// detached thread. The loop joining the vector ends the workers, not those threads, which still
// run when main writes (#200).
// Expected: four data races, main against background, worker against background, background
// against background and worker against worker; main does not race with worker.
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

static void worker(Argument)
{
    shared += 1;
}

int main()
{
    Argument argument;
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i)
        threads.push_back(std::thread(worker, argument));
    for (std::thread& thread : threads)
        thread.join();
    shared += 1;
    return shared;
}
