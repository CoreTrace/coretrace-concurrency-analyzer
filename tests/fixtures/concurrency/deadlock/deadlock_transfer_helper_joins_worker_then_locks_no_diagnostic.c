// SPDX-License-Identifier: Apache-2.0
// The helper joins the worker it is handed, then locks the two accounts it is handed, alice then
// bob; the worker locks bob then alice, but it has ended by then. The worker is still running where
// main calls the helper (#124).
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

static void settleAfter(pthread_t thread, struct Account* from, struct Account* to)
{
    pthread_join(thread, NULL);
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    settleAfter(thread, &alice, &bob);
    return 0;
}
