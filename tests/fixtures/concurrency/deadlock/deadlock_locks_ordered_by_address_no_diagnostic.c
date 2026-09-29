// SPDX-License-Identifier: Apache-2.0
// Both threads take the two locks in address order: an if/else compares the locks' addresses and
// takes the lower one first on either branch. The worker compares the pointers, main compares them
// as integers, the other way round. Whatever the layout, both threads take the same lock first.
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;
static int shared;

static void* worker(void* argument)
{
    (void)argument;
    if (&second < &first)
    {
        pthread_mutex_lock(&second);
        pthread_mutex_lock(&first);
    }
    else
    {
        pthread_mutex_lock(&first);
        pthread_mutex_lock(&second);
    }
    ++shared;
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    if ((uintptr_t)&second >= (uintptr_t)&first)
    {
        pthread_mutex_lock(&first);
        pthread_mutex_lock(&second);
    }
    else
    {
        pthread_mutex_lock(&second);
        pthread_mutex_lock(&first);
    }
    ++shared;
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_join(thread, NULL);
    return shared;
}
