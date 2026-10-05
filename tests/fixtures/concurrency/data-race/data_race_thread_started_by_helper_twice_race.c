// SPDX-License-Identifier: Apache-2.0
// A helper starts the thread; main calls it twice before joining either thread. Both threads run
// at once, and each writes shared (#126).
// Expected: one data race, worker against worker.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void start(pthread_t* thread)
{
    pthread_create(thread, NULL, worker, NULL);
}

int main(void)
{
    pthread_t first;
    pthread_t second;
    start(&first);
    start(&second);
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    return 0;
}
