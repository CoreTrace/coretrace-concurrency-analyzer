// SPDX-License-Identifier: Apache-2.0
// The spawn loop raises its counter before the spawn reads it: the threads fill threads[1..3),
// not threads[0..2), and the loop joining threads[0..2) never joins threads[2].
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
    pthread_t threads[3];
    for (int i = 0; i < 2;)
    {
        ++i;
        pthread_create(&threads[i], NULL, worker, NULL);
    }
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
