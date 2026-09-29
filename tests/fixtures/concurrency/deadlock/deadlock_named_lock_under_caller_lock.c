// SPDX-License-Identifier: Apache-2.0
// A helper locks the global `inner`. The worker calls it while holding `outer`; main calls it
// without, then locks `inner` and `outer` itself. The helper is not always entered holding
// `outer`, so only the worker's call orders `outer` before `inner`.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t outer = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t inner = PTHREAD_MUTEX_INITIALIZER;
static int shared;

static void bump(void)
{
    pthread_mutex_lock(&inner);
    ++shared;
    pthread_mutex_unlock(&inner);
}

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&outer);
    bump();
    pthread_mutex_unlock(&outer);
    return NULL;
}

int main(void)
{
    bump();
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_mutex_lock(&inner);
    pthread_mutex_lock(&outer);
    ++shared;
    pthread_mutex_unlock(&outer);
    pthread_mutex_unlock(&inner);
    pthread_join(thread, NULL);
    return shared;
}
