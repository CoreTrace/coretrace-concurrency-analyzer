// SPDX-License-Identifier: Apache-2.0
// Two workers transfer between alice and bob through a helper that locks the lower address first.
// main locks alice then bob once, before any worker exists. The workers never disagree, and main's
// order runs beside nothing (#138).
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

void transfer(struct Account* from, struct Account* to, int amount)
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

void* payBob(void* argument)
{
    (void)argument;
    transfer(&alice, &bob, 10);
    return NULL;
}

void* payAlice(void* argument)
{
    (void)argument;
    transfer(&bob, &alice, 10);
    return NULL;
}

int main(void)
{
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_lock(&bob.lock);
    alice.balance += 1;
    pthread_mutex_unlock(&bob.lock);
    pthread_mutex_unlock(&alice.lock);

    pthread_t first;
    pthread_t second;
    pthread_create(&first, NULL, payBob, NULL);
    pthread_create(&second, NULL, payAlice, NULL);
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    return alice.balance + bob.balance;
}
