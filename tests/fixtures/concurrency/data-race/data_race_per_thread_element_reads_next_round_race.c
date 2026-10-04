// SPDX-License-Identifier: Apache-2.0
// Thread i reads ids[i + 1], which main writes in the next round while thread i may run (#108).
// Expected: one data race.
#include <pthread.h>
static void* worker(void* argument)
{
    volatile int next = ((int*)argument)[1];
    (void)next;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    int ids[5] = {0};
    for (int i = 0; i < 4; ++i)
    {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
