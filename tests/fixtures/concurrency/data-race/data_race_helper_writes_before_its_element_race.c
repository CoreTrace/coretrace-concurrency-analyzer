// SPDX-License-Identifier: Apache-2.0
// Before starting thread i, a helper given &ids[i] also writes the element before it, which
// thread i - 1 may still be reading (#108).
// Expected: one data race.
#include <pthread.h>
static void set_ids(int* target, int value)
{
    target[0] = value;
    if (value > 0)
        target[-1] = value - 1;
}
static void* worker(void* argument)
{
    volatile int id = *(int*)argument;
    (void)id;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    int ids[4];
    for (int i = 0; i < 4; ++i)
    {
        set_ids(&ids[i], i);
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
