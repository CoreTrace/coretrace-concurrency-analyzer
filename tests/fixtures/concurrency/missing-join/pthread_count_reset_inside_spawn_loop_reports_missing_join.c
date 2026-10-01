// SPDX-License-Identifier: Apache-2.0
// The count of created handles is reset to 0 after the second round: the next rounds store over
// threads[0] and threads[1], and the first two threads are never joined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    int started = 0;
    for (int i = 0; i < 4; i++)
    {
        if (pthread_create(&threads[started], NULL, worker, NULL) == 0)
            ++started;
        if (i == 1)
            started = 0;
    }
    for (int k = 0; k < started; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
