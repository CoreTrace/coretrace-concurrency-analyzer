// SPDX-License-Identifier: Apache-2.0
// Each function tests whether its two lock pointers differ, which they always do, and jumps to its
// own order: first then second in the worker, second then first in the helper main calls. An
// inequality says nothing about which address is lower. Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_t* a = &first;
    pthread_mutex_t* b = &second;
    if (a != b)
        goto distinct;
    pthread_mutex_lock(b);
    pthread_mutex_lock(a);
    pthread_mutex_unlock(a);
    pthread_mutex_unlock(b);
    return NULL;
distinct:
    pthread_mutex_lock(a);
    pthread_mutex_lock(b);
    pthread_mutex_unlock(b);
    pthread_mutex_unlock(a);
    return NULL;
}

static void backward(void)
{
    pthread_mutex_t* a = &first;
    pthread_mutex_t* b = &second;
    if (a != b)
        goto distinct;
    pthread_mutex_lock(a);
    pthread_mutex_lock(b);
    pthread_mutex_unlock(b);
    pthread_mutex_unlock(a);
    return;
distinct:
    pthread_mutex_lock(b);
    pthread_mutex_lock(a);
    pthread_mutex_unlock(a);
    pthread_mutex_unlock(b);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    backward();
    pthread_join(thread, NULL);
    return 0;
}
