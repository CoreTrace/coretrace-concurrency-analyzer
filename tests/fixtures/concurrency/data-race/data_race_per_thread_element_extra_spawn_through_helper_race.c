// SPDX-License-Identifier: Apache-2.0
// Besides the loop, a helper starts the same worker on &results[0], which loop thread 0 writes at
// the same time (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
static void spawn_one(pthread_t* thread, void* (*entry)(void*), void* argument)
{
    pthread_create(thread, NULL, entry, argument);
}
int main(void)
{
    pthread_t threads[4];
    pthread_t extra;
    spawn_one(&extra, worker, &results[0]);
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    pthread_join(extra, NULL);
    return results[0];
}
