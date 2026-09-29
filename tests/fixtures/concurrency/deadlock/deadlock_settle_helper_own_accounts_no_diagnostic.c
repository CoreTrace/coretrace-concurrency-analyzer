// SPDX-License-Identifier: Apache-2.0
// The helper locks the account it is handed then the ledger, and later the ledger then the account.
// Each thread settles its own account: the ledger is the only lock the two threads share, so no
// thread can hold a lock the other waits for while waiting itself.
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
static pthread_mutex_t ledger = PTHREAD_MUTEX_INITIALIZER;
static int entries;

static void settle(struct Account* account)
{
    pthread_mutex_lock(&account->lock);
    pthread_mutex_lock(&ledger);
    ++entries;
    pthread_mutex_unlock(&ledger);
    pthread_mutex_unlock(&account->lock);

    pthread_mutex_lock(&ledger);
    pthread_mutex_lock(&account->lock);
    account->balance -= 1;
    pthread_mutex_unlock(&account->lock);
    pthread_mutex_unlock(&ledger);
}

static void* settleBob(void* argument)
{
    (void)argument;
    settle(&bob);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, settleBob, NULL);
    settle(&alice);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance + entries;
}
