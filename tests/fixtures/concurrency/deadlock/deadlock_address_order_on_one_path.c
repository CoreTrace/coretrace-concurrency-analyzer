// SPDX-License-Identifier: Apache-2.0
// Only the backward call orders the two locks by address; the forward call always takes first,
// then second. When second is at the lower address, the worker's backward call and main's forward
// call take the locks in opposite orders. Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void work(int forward)
{
    if (!forward)
    {
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
    }
    else
    {
        pthread_mutex_lock(&first);
        pthread_mutex_lock(&second);
    }
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
}

static void* backward(void* argument)
{
    (void)argument;
    work(0);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    work(1);
    pthread_join(thread, NULL);
    return 0;
}
