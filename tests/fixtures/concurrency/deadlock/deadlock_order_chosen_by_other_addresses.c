// SPDX-License-Identifier: Apache-2.0
// An address comparison chooses the order, but of two pointers that are not the locks: the worker
// passes them one way and main the other, so they take the two locks in opposite orders.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;
static int low;
static int high;

static void work(const int* left, const int* right)
{
    if (left < right)
    {
        pthread_mutex_lock(&first);
        pthread_mutex_lock(&second);
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
    work(&high, &low);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    work(&low, &high);
    pthread_join(thread, NULL);
    return 0;
}
