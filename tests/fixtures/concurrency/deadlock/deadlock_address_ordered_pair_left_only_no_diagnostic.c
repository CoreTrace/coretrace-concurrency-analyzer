// SPDX-License-Identifier: Apache-2.0
// deadlock_address_ordered_pair_entered_and_left_no_diagnostic.c without registry: the pair is left
// (ledger), never entered (#138).
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
    transfer(&alice, &bob);
    pthread_join(thread, NULL);
    return 0;
}
