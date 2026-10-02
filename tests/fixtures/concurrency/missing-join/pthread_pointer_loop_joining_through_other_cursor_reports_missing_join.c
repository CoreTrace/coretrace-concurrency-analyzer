// SPDX-License-Identifier: Apache-2.0
// The pointer loop walks threads, but joins through another local that always points at threads[0]:
// the thread in threads[1] is never joined, and runs when main writes.
// Expected: one data race and one missing join.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

int main(void)
{
    pthread_t threads[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    pthread_t* first = threads;
    for (pthread_t* handle = threads; handle != threads + 2; ++handle)
    {
        pthread_join(*first, NULL);
        first = threads;
    }
    shared += 2;
    return shared;
}
