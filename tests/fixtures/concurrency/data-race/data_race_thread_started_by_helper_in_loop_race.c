// SPDX-License-Identifier: Apache-2.0
// A helper starts the thread; main calls it in a loop and joins the threads after the loop. Both
// threads run at once, and each writes shared (#126).
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
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i)
        start(&threads[i]);
    for (int i = 0; i < 2; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
