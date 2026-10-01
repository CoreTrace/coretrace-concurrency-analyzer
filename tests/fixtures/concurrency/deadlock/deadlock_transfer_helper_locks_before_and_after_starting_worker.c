// SPDX-License-Identifier: Apache-2.0
// The helper transfers from the first account it is handed to the second before it starts the
// worker, and again while the worker runs; the worker locks bob then alice. At main's call the two
// transfers are one order, alice then bob, and the second one runs beside the worker (#124).
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

static void transferBeforeAndWhileAuditing(struct Account* from, struct Account* to)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);

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
    transferBeforeAndWhileAuditing(&alice, &bob);
    return 0;
}
