// SPDX-License-Identifier: Apache-2.0
// The inversion is expressed entirely through helpers: neither thread names a lock primitive
// directly. Without a summary of what `take` does to the lock it is handed, the acquisition
// vanishes at the return and the cycle is invisible.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second_lock = PTHREAD_MUTEX_INITIALIZER;

static void take(pthread_mutex_t* lock)
{
    pthread_mutex_lock(lock);
}

static void give(pthread_mutex_t* lock)
{
    pthread_mutex_unlock(lock);
}

static void* ascending(void* argument)
{
    (void)argument;
    take(&first_lock);
    take(&second_lock);
    give(&second_lock);
    give(&first_lock);
    return NULL;
}

static void* descending(void* argument)
{
    (void)argument;
    take(&second_lock);
    take(&first_lock);
    give(&first_lock);
    give(&second_lock);
    return NULL;
}

int main(void)
{
    pthread_t ascendingThread;
    pthread_t descendingThread;

    pthread_create(&ascendingThread, NULL, ascending, NULL);
    pthread_create(&descendingThread, NULL, descending, NULL);
    pthread_join(ascendingThread, NULL);
    pthread_join(descendingThread, NULL);
    return 0;
}
