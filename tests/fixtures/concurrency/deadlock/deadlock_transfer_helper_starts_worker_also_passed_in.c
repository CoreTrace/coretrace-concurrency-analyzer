// SPDX-License-Identifier: Apache-2.0
// The helper starts the worker, then locks the two accounts it is handed; the worker locks bob then
// alice. main first hands the helper alice then bob, before any worker runs, then starts a worker
// of its own and hands the helper two other accounts. The worker is thus both passed to the helper
// and started by it, and the one it starts runs beside the first transfer (#124).
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
static struct Account carol = {PTHREAD_MUTEX_INITIALIZER, 100};
static struct Account dave = {PTHREAD_MUTEX_INITIALIZER, 100};

static void* backward(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&bob.lock);
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_unlock(&alice.lock);
    pthread_mutex_unlock(&bob.lock);
    return NULL;
}

static void transferWhileAuditing(struct Account* from, struct Account* to)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
    pthread_join(thread, NULL);
}

int main(void)
{
    transferWhileAuditing(&alice, &bob);
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    transferWhileAuditing(&carol, &dave);
    pthread_join(thread, NULL);
    return 0;
}
