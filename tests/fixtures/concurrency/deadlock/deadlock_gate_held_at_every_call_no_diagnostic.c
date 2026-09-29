// SPDX-License-Identifier: Apache-2.0
// deadlock_gate_held_at_concurrent_calls_only_no_diagnostic.c without main's call to forward before
// the worker exists (#136).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void forward(void)
{
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
}

static void backward(void)
{
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
}

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    forward();
    pthread_mutex_unlock(&gate);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_mutex_lock(&gate);
    backward();
    pthread_mutex_unlock(&gate);
    pthread_join(thread, NULL);
    return 0;
}
