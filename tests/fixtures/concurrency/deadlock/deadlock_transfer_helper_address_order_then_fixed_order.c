// SPDX-License-Identifier: Apache-2.0
// The helper takes the two accounts' locks lower address first, then again in a fixed order. When
// second is at the lower address, a thread in the fixed part holds first and waits for second,
// held by the other thread in the address-ordered part. Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account first = {PTHREAD_MUTEX_INITIALIZER, 0};
static struct Account second = {PTHREAD_MUTEX_INITIALIZER, 0};

static void settle(struct Account* a, struct Account* b)
{
    if (a < b)
    {
        pthread_mutex_lock(&a->lock);
        pthread_mutex_lock(&b->lock);
    }
    else
    {
        pthread_mutex_lock(&b->lock);
        pthread_mutex_lock(&a->lock);
    }
    ++a->balance;
    pthread_mutex_unlock(&b->lock);
    pthread_mutex_unlock(&a->lock);

    pthread_mutex_lock(&a->lock);
    pthread_mutex_lock(&b->lock);
    --b->balance;
    pthread_mutex_unlock(&b->lock);
    pthread_mutex_unlock(&a->lock);
}

static void* worker(void* argument)
{
    (void)argument;
    settle(&first, &second);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    settle(&first, &second);
    pthread_join(thread, NULL);
    return 0;
}
