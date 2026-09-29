// SPDX-License-Identifier: Apache-2.0
// main holds alice's lock and calls a helper that locks the account it is handed: the same
// non-recursive mutex, taken twice by one thread. The worker calls the helper without holding
// anything, so the helper is not always entered holding the lock.
// Expected: one deadlock, a reacquisition.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account alice = {PTHREAD_MUTEX_INITIALIZER, 100};

static void deposit(struct Account* account, int amount)
{
    pthread_mutex_lock(&account->lock);
    account->balance += amount;
    pthread_mutex_unlock(&account->lock);
}

static void* payAlice(void* argument)
{
    (void)argument;
    deposit(&alice, 1);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, payAlice, NULL);
    pthread_mutex_lock(&alice.lock);
    deposit(&alice, 10);
    pthread_mutex_unlock(&alice.lock);
    pthread_join(thread, NULL);
    return alice.balance;
}
