// SPDX-License-Identifier: Apache-2.0
// main reaches the helper through a relay. The helper transfers from the first account it is
// handed to the second before it starts the worker, then again, through two more calls, while the
// worker runs; the worker locks bob then alice. Each caller is defined before its callee, so the
// relay learns that the second transfer runs beside the worker one round after the first (#124).
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

static void relay(struct Account* from, struct Account* to);
static void transferTwice(struct Account* from, struct Account* to);
static void lockBoth(struct Account* from, struct Account* to);
static void lockPair(struct Account* from, struct Account* to);

static void* backward(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&bob.lock);
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_unlock(&alice.lock);
    pthread_mutex_unlock(&bob.lock);
    return NULL;
}

int main(void)
{
    relay(&alice, &bob);
    return 0;
}

static void relay(struct Account* from, struct Account* to)
{
    transferTwice(from, to);
}

static void transferTwice(struct Account* from, struct Account* to)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);

    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    lockBoth(from, to);
    pthread_join(thread, NULL);
}

static void lockBoth(struct Account* from, struct Account* to)
{
    lockPair(from, to);
}

static void lockPair(struct Account* from, struct Account* to)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}
