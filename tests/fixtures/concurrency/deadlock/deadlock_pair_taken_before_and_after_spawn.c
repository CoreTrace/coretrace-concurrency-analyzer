// SPDX-License-Identifier: Apache-2.0
// main takes first then second once before the worker exists and once while it runs; the worker
// takes second then first (#127).
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    return NULL;
}

int main(void)
{
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);

    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);

    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_join(thread, NULL);
    return 0;
}
