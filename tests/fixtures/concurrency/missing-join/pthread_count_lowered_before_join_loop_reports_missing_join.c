// SPDX-License-Identifier: Apache-2.0
// The handles of the created threads fill threads[0..started), but the count is lowered before
// the join loop reads it: the last created thread is never joined.
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
        if (pthread_create(&threads[started], NULL, worker, NULL) == 0)
            ++started;
    }
    --started;
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return started;
}
