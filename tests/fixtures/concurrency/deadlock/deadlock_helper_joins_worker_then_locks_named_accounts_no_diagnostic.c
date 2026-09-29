// SPDX-License-Identifier: Apache-2.0
// deadlock_transfer_helper_joins_worker_then_locks_no_diagnostic.c with the helper locking alice
// and bob by name instead of through its parameters (#124).
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

static void* backward(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&bob.lock);
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_unlock(&alice.lock);
    pthread_mutex_unlock(&bob.lock);
    return NULL;
}

static void settleAfter(pthread_t thread)
{
    pthread_join(thread, NULL);
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_lock(&bob.lock);
    pthread_mutex_unlock(&bob.lock);
    pthread_mutex_unlock(&alice.lock);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    settleAfter(thread);
    return 0;
}
