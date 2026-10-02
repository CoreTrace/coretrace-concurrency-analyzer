// SPDX-License-Identifier: Apache-2.0
// Each worker reads its own slot of a local array. The handles of the created threads fill
// threads[0..started), and a loop joins them all before the function returns: every worker has
// ended, so the frame outlives them all (#157).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

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
        pthread_join(threads[k], NULL);
}
