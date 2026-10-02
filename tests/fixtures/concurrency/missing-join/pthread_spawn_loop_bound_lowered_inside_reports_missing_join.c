// SPDX-License-Identifier: Apache-2.0
// The spawn loop runs while i < n, and lowers n to 2 after its third spawn: three threads start,
// and the loop joining threads[0..n) leaves threads[2] unjoined.
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
    int n = 4;
    for (int i = 0; i < n; i++)
    {
        pthread_create(&threads[i], NULL, worker, NULL);
        if (i == 2)
            n = 2;
    }
    for (int k = 0; k < n; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
