// SPDX-License-Identifier: Apache-2.0
// Before the spawn loop, main hands &results[0] to a helper that stashes it under a lock; the
// observer writes through the stashed address while thread 0 writes results[0]. No thread runs
// at the call, so only the element's identity can show the race (#108).
// Expected: one data race.
#include <pthread.h>

static int results[4];
static int* stashed;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;

static void stash(int* element)
{
    pthread_mutex_lock(&gate);
    stashed = element;
    pthread_mutex_unlock(&gate);
}

static void* observer(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    int* seen = stashed;
    pthread_mutex_unlock(&gate);
    if (seen != NULL)
        *seen = 0;
    return NULL;
}

static void* worker(void* argument)
{
    int* element = argument;
    *element = 42;
    return NULL;
}

int main(void)
{
    pthread_t watcher;
    pthread_t threads[4];
    pthread_create(&watcher, NULL, observer, NULL);
    stash(&results[0]);
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    pthread_join(watcher, NULL);
    return 0;
}
