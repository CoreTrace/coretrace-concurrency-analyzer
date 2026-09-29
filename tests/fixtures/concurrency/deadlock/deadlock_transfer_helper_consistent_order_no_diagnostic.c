// SPDX-License-Identifier: Apache-2.0
// Both threads transfer from alice to bob through the helper, so both lock alice first.
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

static void* payBob(void* argument)
{
    (void)argument;
    transfer(&alice, &bob, 10);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, payBob, NULL);
    transfer(&alice, &bob, 10);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance;
}
