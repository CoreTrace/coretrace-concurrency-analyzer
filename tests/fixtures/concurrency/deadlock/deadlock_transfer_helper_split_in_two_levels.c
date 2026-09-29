// SPDX-License-Identifier: Apache-2.0
// The transfer locks the payer's account, then calls a second helper that locks the payee's while
// the first lock is still held. main and the worker transfer in opposite directions.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account alice = {PTHREAD_MUTEX_INITIALIZER, 100};
static struct Account bob = {PTHREAD_MUTEX_INITIALIZER, 100};

static void credit(struct Account* to, int amount)
{
    pthread_mutex_lock(&to->lock);
    to->balance += amount;
    pthread_mutex_unlock(&to->lock);
}

static void transfer(struct Account* from, struct Account* to, int amount)
{
    pthread_mutex_lock(&from->lock);
    from->balance -= amount;
    credit(to, amount);
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
    transfer(&alice, &bob, 10);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance;
}
