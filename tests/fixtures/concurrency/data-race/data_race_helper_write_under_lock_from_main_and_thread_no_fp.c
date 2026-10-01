// SPDX-License-Identifier: Apache-2.0
// main and the worker thread both call store(), which writes `shared` while holding `lock`: the
// lock orders the two writes (#155).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void store(void)
{
    pthread_mutex_lock(&lock);
    shared = 42;
    pthread_mutex_unlock(&lock);
}

static void* worker(void* argument)
{
    (void)argument;
    store();
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    store();
    pthread_join(thread, NULL);
    return shared;
}
