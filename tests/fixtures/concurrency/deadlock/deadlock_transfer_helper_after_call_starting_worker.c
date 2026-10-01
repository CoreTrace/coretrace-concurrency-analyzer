// SPDX-License-Identifier: Apache-2.0
// The helper calls a function that starts the worker and returns without waiting for it, then
// locks the two accounts it is handed, alice then bob; the worker locks bob then alice (#124).
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

static pthread_t worker;

static void startWorker(void)
{
    pthread_create(&worker, NULL, backward, NULL);
}

static void transferWhileAuditing(struct Account* from, struct Account* to)
{
    startWorker();
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
    pthread_join(worker, NULL);
}

int main(void)
{
    transferWhileAuditing(&alice, &bob);
    return 0;
}
