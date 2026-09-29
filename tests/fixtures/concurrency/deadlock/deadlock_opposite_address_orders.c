// SPDX-License-Identifier: Apache-2.0
// Both threads order the two locks by address, but the worker takes the lower one first and main
// the higher one: whatever the layout, their orders are opposite. Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;
static int shared;

static void* worker(void* argument)
{
    (void)argument;
    if (&first < &second)
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
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    if (&first < &second)
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
    pthread_join(thread, NULL);
    return shared;
}
