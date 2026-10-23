// SPDX-License-Identifier: Apache-2.0
// A loop hands each slot of the array to a helper joining the pthread_t its argument points to:
// every reader ends before main writes (#213).
// Expected: no diagnostic.
#include <pthread.h>

static int shared;

static void* reader(void*)
{
    int seen = shared;
    (void)seen;
    return nullptr;
}

static void stop(pthread_t* handle)
{
    pthread_join(*handle, nullptr);
}

int main()
{
    pthread_t threads[3];
    for (int i = 0; i < 3; ++i)
        pthread_create(&threads[i], nullptr, reader, nullptr);
    for (int i = 0; i < 3; ++i)
        stop(&threads[i]);
    shared = 1;
    return shared;
}
