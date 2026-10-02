// SPDX-License-Identifier: Apache-2.0
// The handles are stored at threads[i] while started counts the successful creations only: once a
// creation fails, its slot is not counted, and the last handle lies at or past started, where the
// join loop never goes.
// Expected: one missing join.
#include <pthread.h>

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
        if (pthread_create(&threads[i], NULL, worker, NULL) == 0)
            ++started;
    }
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return started;
}
