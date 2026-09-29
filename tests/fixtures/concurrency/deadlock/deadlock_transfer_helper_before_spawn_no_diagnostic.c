// SPDX-License-Identifier: Apache-2.0
// main's transfer from alice to bob is over before the worker, which transfers the other way, is
// created: the two opposite orders never run at the same time.
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

static void transfer(struct Account* from, struct Account* to, int amount)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
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
    transfer(&alice, &bob, 10);
    pthread_t thread;
    pthread_create(&thread, NULL, payAlice, NULL);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance;
}
