// SPDX-License-Identifier: Apache-2.0
// Both accounts live in a local of main, handed to the worker by pointer. The two threads transfer
// in opposite directions through the same helper, which locks the payer's account, then the
// payee's.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

struct Bank
{
    struct Account alice;
    struct Account bob;
};

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
    struct Bank* bank = argument;
    transfer(&bank->bob, &bank->alice, 10);
    return NULL;
}

int main(void)
{
    struct Bank bank = {{PTHREAD_MUTEX_INITIALIZER, 100}, {PTHREAD_MUTEX_INITIALIZER, 100}};
    pthread_t thread;
    pthread_create(&thread, NULL, payAlice, &bank);
    transfer(&bank.alice, &bank.bob, 10);
    pthread_join(thread, NULL);
    return bank.alice.balance + bank.bob.balance;
}
