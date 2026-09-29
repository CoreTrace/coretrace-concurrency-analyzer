// SPDX-License-Identifier: Apache-2.0
// Only main calls the transfer helper, in both directions, one call after the other. The worker
// touches neither account, so no second thread ever waits for one of the two locks.
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account alice = {PTHREAD_MUTEX_INITIALIZER, 100};
static struct Account bob = {PTHREAD_MUTEX_INITIALIZER, 100};
static int ticks;

static void transfer(struct Account* from, struct Account* to, int amount)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    from->balance -= amount;
    to->balance += amount;
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

static void* tick(void* argument)
{
    (void)argument;
    __atomic_fetch_add(&ticks, 1, __ATOMIC_RELAXED);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, tick, NULL);
    transfer(&alice, &bob, 10);
    transfer(&bob, &alice, 10);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance;
}
