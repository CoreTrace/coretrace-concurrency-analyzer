// SPDX-License-Identifier: Apache-2.0
// deadlock_transfer_helper_joins_worker_then_locks_no_diagnostic.c with main joining the worker
// before the call instead of the helper (#124).
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

static void transfer(struct Account* from, struct Account* to)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    pthread_join(thread, NULL);
    transfer(&alice, &bob);
    return 0;
}
