// SPDX-License-Identifier: Apache-2.0
// The spawn loop stores each handle at threads[i] but raises started on even rounds only, so the
// loop joining threads[0..started) stops at 2 and two threads are never joined.
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
        pthread_create(&threads[i], NULL, worker, NULL);
        if (i % 2 == 0)
            ++started;
    }
    for (int k = 0; k < started; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
