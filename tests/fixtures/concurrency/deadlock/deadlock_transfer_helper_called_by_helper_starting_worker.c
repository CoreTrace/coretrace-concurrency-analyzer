// SPDX-License-Identifier: Apache-2.0
// deadlock_transfer_helper_starts_worker_then_locks.c one level deeper: the helper starts the
// worker, then hands the two accounts to another helper that locks them (#124).
// Expected: one deadlock.
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

static void transferWhileAuditing(struct Account* from, struct Account* to)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    transfer(from, to);
    pthread_join(thread, NULL);
}

int main(void)
{
    transferWhileAuditing(&alice, &bob);
    return 0;
}
