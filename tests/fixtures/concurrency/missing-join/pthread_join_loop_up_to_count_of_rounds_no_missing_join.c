// SPDX-License-Identifier: Apache-2.0
// The spawn loop stores each handle at threads[i] and raises started once every round, so started
// ends at 4: the loop joining threads[0..started) joins every thread (#157).
// Expected: no diagnostic.
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
        ++started;
    }
    for (int k = 0; k < started; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
