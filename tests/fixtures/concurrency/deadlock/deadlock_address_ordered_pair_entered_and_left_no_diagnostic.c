// SPDX-License-Identifier: Apache-2.0
// The helper locks the two accounts it is handed lower address first, then ledger while it holds
// both. main holds registry around its call. Every thread takes registry, then the accounts in
// address order, then ledger (#138).
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
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ledger = PTHREAD_MUTEX_INITIALIZER;

static void transfer(struct Account* from, struct Account* to)
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
    pthread_mutex_lock(&ledger);
    pthread_mutex_unlock(&ledger);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

static void* payAlice(void* argument)
{
    (void)argument;
    transfer(&bob, &alice);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, payAlice, NULL);
    pthread_mutex_lock(&registry);
    transfer(&alice, &bob);
    pthread_mutex_unlock(&registry);
    pthread_join(thread, NULL);
    return 0;
}
