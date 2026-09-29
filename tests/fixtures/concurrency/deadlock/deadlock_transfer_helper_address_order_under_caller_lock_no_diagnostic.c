// SPDX-License-Identifier: Apache-2.0
// The helper locks the account at the lower address first. main holds `registry` around its call,
// the worker calls it without: `registry` is taken before both accounts, and never after one, so
// the address order still decides every thread's order.
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
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;

static void transfer(struct Account* from, struct Account* to, int amount)
{
    if (from < to)
    {
        pthread_mutex_lock(&from->lock);
        pthread_mutex_lock(&to->lock);
    }
    else
    {
        pthread_mutex_lock(&to->lock);
        pthread_mutex_lock(&from->lock);
    }
    from->balance -= amount;
    to->balance += amount;
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

static void* payAlice(void* argument)
{
    (void)argument;
    transfer(&bob, &alice, 10);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, payAlice, NULL);
    pthread_mutex_lock(&registry);
    transfer(&alice, &bob, 10);
    pthread_mutex_unlock(&registry);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance;
}
