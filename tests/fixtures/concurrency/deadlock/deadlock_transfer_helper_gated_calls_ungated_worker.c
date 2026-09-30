// SPDX-License-Identifier: Apache-2.0
// deadlock_transfer_helper_gated_calls_no_diagnostic.c with the gate removed from the worker:
// main's gated transfer, holding gate and alice, and the worker's transfer, holding bob, can wait
// for each other (#127).
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
    transfer(&bob, &alice, 10);
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
