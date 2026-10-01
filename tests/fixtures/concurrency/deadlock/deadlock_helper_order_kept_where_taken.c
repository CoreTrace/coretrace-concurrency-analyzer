// SPDX-License-Identifier: Apache-2.0
// forward takes first then second, and main takes second then first while the worker runs. The
// worker reaches forward through forwardFor, holding registry around the call, and forwardFor
// holds the lock of the account it is handed. Every call to forward holds the same locks, none of
// which main holds: the calls tell nothing forward does not know, so the deadlock is reported where
// forward takes its locks (#136).
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
static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void forward(void)
{
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
}

static void forwardFor(struct Account* account)
{
    pthread_mutex_lock(&account->lock);
    forward();
    pthread_mutex_unlock(&account->lock);
}

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&registry);
    forwardFor(&alice);
    pthread_mutex_unlock(&registry);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    pthread_join(thread, NULL);
    return 0;
}
