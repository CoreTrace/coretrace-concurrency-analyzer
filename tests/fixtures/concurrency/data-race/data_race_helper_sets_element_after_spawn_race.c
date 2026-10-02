// SPDX-License-Identifier: Apache-2.0
// A helper writes ids[i] after thread i starts, while thread i reads it (#108).
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
        ids[i] = 0;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
        set_id(&ids[i], i);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
