// SPDX-License-Identifier: Apache-2.0
// The two inverted transfers that can run together are both made while holding `gate`, which
// serializes them. main also transfers once without `gate`, before the worker exists, so the
// helper is not always entered holding `gate`: only the calls themselves show it.
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
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&gate);
    transfer(&bob, &alice, 10);
    pthread_mutex_unlock(&gate);
    return NULL;
}

int main(void)
{
    transfer(&alice, &bob, 1);
    pthread_t thread;
    pthread_create(&thread, NULL, payAlice, NULL);
    pthread_mutex_lock(&gate);
    transfer(&alice, &bob, 10);
    pthread_mutex_unlock(&gate);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance;
}
