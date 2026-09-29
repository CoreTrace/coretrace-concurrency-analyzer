// SPDX-License-Identifier: Apache-2.0
// The if/else of address ordering, but a flag chooses the order: the worker passes 0 and main
// passes 1, so they take the two locks in opposite orders. Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void work(int forward)
{
    if (forward)
    {
        pthread_mutex_lock(&first);
        pthread_mutex_lock(&second);
        pthread_mutex_unlock(&second);
        pthread_mutex_unlock(&first);
    }
    else
    {
        pthread_mutex_lock(&second);
        pthread_mutex_lock(&first);
        pthread_mutex_unlock(&first);
        pthread_mutex_unlock(&second);
    }
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
