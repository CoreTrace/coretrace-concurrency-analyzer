// SPDX-License-Identifier: Apache-2.0
// Each worker publishes its element's address under a lock; an observer thread writes through it
// while the worker writes its element (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static int* current;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static void* worker(void* argument)
{
    int* slot = argument;
    pthread_mutex_lock(&gate);
    current = slot;
    pthread_mutex_unlock(&gate);
    *slot = 42;
    return NULL;
}
static void* observer(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    int* seen = current;
    pthread_mutex_unlock(&gate);
    if (seen != NULL)
        *seen = 0;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    pthread_t watch;
    pthread_create(&watch, NULL, observer, NULL);
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    pthread_join(watch, NULL);
    return 0;
}
