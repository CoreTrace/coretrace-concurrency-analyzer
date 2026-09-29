// SPDX-License-Identifier: Apache-2.0
// Hand-over-hand locking by recursion: each call locks its account, reads it, and recurses on the
// next one, in the same direction in both threads. Nothing is written and the order is the same
// everywhere, so there is neither a race nor a deadlock. The recursion moves the pointer at every
// call, and the analysis must still terminate (#117).
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account accounts[4] = {{PTHREAD_MUTEX_INITIALIZER, 1},
                                     {PTHREAD_MUTEX_INITIALIZER, 2},
                                     {PTHREAD_MUTEX_INITIALIZER, 3},
                                     {PTHREAD_MUTEX_INITIALIZER, 4}};

static int sumFrom(struct Account* account, int remaining)
{
    pthread_mutex_lock(&account->lock);
    int total = account->balance;
    if (remaining > 1)
        total += sumFrom(account + 1, remaining - 1);
    pthread_mutex_unlock(&account->lock);
    return total;
}

static void* audit(void* argument)
{
    (void)argument;
    return (void*)(long)sumFrom(&accounts[0], 4);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, audit, NULL);
    int total = sumFrom(&accounts[0], 4);
    pthread_join(thread, NULL);
    return total;
}
