// SPDX-License-Identifier: Apache-2.0
// Before starting thread i, a helper writes ids[i - 1], which thread i - 1 may still be reading
// (#108).
// Expected: one data race.
#include <pthread.h>
static void set_id(int* target, int value)
{
    *target = value;
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
        ids[i] = i;
        if (i > 0)
            set_id(&ids[i - 1], i);
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
