// SPDX-License-Identifier: Apache-2.0
// deadlock_address_order_on_one_path.c with the forward call taking second, then first. When first
// is at the lower address, the worker's backward call holds first while main's forward call holds
// second. The cycle's order taken by address now comes before its fixed one (#138).
// Expected: one deadlock.
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
        pthread_mutex_lock(&second);
        pthread_mutex_lock(&first);
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
