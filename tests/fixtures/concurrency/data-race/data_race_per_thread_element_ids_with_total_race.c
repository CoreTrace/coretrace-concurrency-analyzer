// SPDX-License-Identifier: Apache-2.0
// on total: every worker increments it. main writes ids[i] before thread i starts, and thread i
// reads only ids[i]: no race on ids (#108).
// Expected: one data race.
#include <pthread.h>
static long total;
static void* worker(void* argument)
{
    int id = *(int*)argument;
    total += id;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    int ids[4];
    for (int i = 0; i < 4; ++i)
    {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return (int)total;
}
