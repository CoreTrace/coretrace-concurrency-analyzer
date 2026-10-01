// SPDX-License-Identifier: Apache-2.0
// A successful creation raises the count only when `verbose` is set: otherwise the next creation
// stores over the handle of the thread just started, which is lost.
// Expected: one missing join.
#include <pthread.h>

extern int verbose;

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    int started = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (pthread_create(&threads[started], NULL, worker, NULL) == 0)
        {
            if (verbose)
                ++started;
        }
    }
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return started;
}
