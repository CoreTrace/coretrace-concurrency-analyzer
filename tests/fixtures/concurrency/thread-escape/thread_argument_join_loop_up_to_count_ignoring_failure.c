// SPDX-License-Identifier: Apache-2.0
// Each worker reads its own slot of a local array. The handles of the created threads fill
// threads[0..started), and a loop joins them all, but only reports a failed join and goes on: a
// worker whose join failed may still read the frame after the function has returned. Every
// handle is joined.
// Expected: one thread argument escape.
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>

static void* worker(void* argument)
{
    volatile int id = *(int*)argument;
    (void)id;
    return NULL;
}

void run_workers(void)
{
    pthread_t threads[4];
    int ids[4];
    int started = 0;
    for (int i = 0; i < 4; ++i)
    {
        ids[started] = i;
        if (pthread_create(&threads[started], NULL, worker, &ids[started]) == 0)
            ++started;
    }
    for (int k = 0; k < started; ++k)
        if (pthread_join(threads[k], NULL) != 0)
            perror("pthread_join");
}
