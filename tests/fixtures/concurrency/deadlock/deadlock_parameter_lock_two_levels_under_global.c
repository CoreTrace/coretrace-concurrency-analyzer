// SPDX-License-Identifier: Apache-2.0
// main holds `registry` around a call to a helper that reaches the account's lock one call further
// down; main also calls that helper without `registry`. The auditor locks the account, then
// `registry`.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account alice = {PTHREAD_MUTEX_INITIALIZER, 100};
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static int audits;

static void deposit(struct Account* account, int amount)
{
    pthread_mutex_lock(&account->lock);
    account->balance += amount;
    pthread_mutex_unlock(&account->lock);
}

static void payday(struct Account* account)
{
    deposit(account, 10);
}

static void* audit(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_lock(&registry);
    ++audits;
    pthread_mutex_unlock(&registry);
    pthread_mutex_unlock(&alice.lock);
    return NULL;
}

int main(void)
{
    payday(&alice);
    pthread_t thread;
    pthread_create(&thread, NULL, audit, NULL);
    pthread_mutex_lock(&registry);
    payday(&alice);
    pthread_mutex_unlock(&registry);
    pthread_join(thread, NULL);
    return alice.balance + audits;
}
