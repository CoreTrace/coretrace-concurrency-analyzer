// SPDX-License-Identifier: Apache-2.0
// deadlock_transfer_helper_starts_worker_then_locks.c with the helper locking alice and bob by name
// instead of through its parameters (#124).
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

static void transferWhileAuditing(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_lock(&bob.lock);
    pthread_mutex_unlock(&bob.lock);
    pthread_mutex_unlock(&alice.lock);
    pthread_join(thread, NULL);
}

int main(void)
{
    transferWhileAuditing();
    return 0;
}
