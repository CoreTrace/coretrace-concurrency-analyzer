// SPDX-License-Identifier: Apache-2.0
// The helper is handed the two mutexes themselves, not objects holding them, and locks them in the
// order it is given them. main and the worker give them in opposite orders.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void lockPair(pthread_mutex_t* outer, pthread_mutex_t* inner)
{
    pthread_mutex_lock(outer);
    pthread_mutex_lock(inner);
    pthread_mutex_unlock(inner);
    pthread_mutex_unlock(outer);
}

static void* worker(void* argument)
{
    (void)argument;
    lockPair(&second, &first);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    lockPair(&first, &second);
    pthread_join(thread, NULL);
    return 0;
}
