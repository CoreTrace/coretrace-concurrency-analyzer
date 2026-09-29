// SPDX-License-Identifier: Apache-2.0
// main holds `registry` around a call to a helper that locks the account it is handed; the auditor
// locks the same account directly, then `registry`. main also calls the helper once without
// `registry`, so the helper is not always entered holding it. The lock is the account's second
// field: the helper takes it past the pointer it is handed, where the auditor names it too.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    int balance;
    pthread_mutex_t lock;
};

static struct Account alice = {100, PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static int audits;

static void deposit(struct Account* account, int amount)
{
    pthread_mutex_lock(&account->lock);
    account->balance += amount;
    pthread_mutex_unlock(&account->lock);
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
    deposit(&alice, 1);
    pthread_t thread;
    pthread_create(&thread, NULL, audit, NULL);
    pthread_mutex_lock(&registry);
    deposit(&alice, 10);
    pthread_mutex_unlock(&registry);
    pthread_join(thread, NULL);
    return alice.balance + audits;
}
