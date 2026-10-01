// SPDX-License-Identifier: Apache-2.0
// Two different threads both call store(), which writes `shared`, and each holds `lock` around
// the call: the lock orders the two writes (#155).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void store(void)
{
    shared = 42;
}

static void* first(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&lock);
    store();
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&lock);
    store();
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(void)
{
    pthread_t firstThread;
    pthread_t secondThread;
    pthread_create(&firstThread, NULL, first, NULL);
    pthread_create(&secondThread, NULL, second, NULL);
    pthread_join(firstThread, NULL);
    pthread_join(secondThread, NULL);
    return shared;
}
