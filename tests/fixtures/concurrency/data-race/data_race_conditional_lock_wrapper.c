// SPDX-License-Identifier: Apache-2.0
// The helper only takes the lock on one branch, so the caller cannot be told it holds it.
// Claiming otherwise would silence a real race, which is the failure worth avoiding.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static int shared_total = 0;

static void take_sometimes(pthread_mutex_t* lock, int should_take)
{
    if (should_take)
        pthread_mutex_lock(lock);
}

static void* worker(void* argument)
{
    (void)argument;
    take_sometimes(&guard, 0);
    shared_total = shared_total + 1;
    return NULL;
}

int main(void)
{
    pthread_t firstThread;
    pthread_t secondThread;

    pthread_create(&firstThread, NULL, worker, NULL);
    pthread_create(&secondThread, NULL, worker, NULL);
    pthread_join(firstThread, NULL);
    pthread_join(secondThread, NULL);
    return 0;
}
