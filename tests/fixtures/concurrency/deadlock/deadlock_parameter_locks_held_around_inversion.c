// SPDX-License-Identifier: Apache-2.0
// Each helper holds the lock of the account it is handed around its own order of two global locks,
// and the two orders are opposite. The helpers are handed different accounts, so the account lock
// serializes nothing: it is a different lock in each thread, though both name it after their first
// parameter.
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
static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;
static int shared;

static void forward(struct Account* account)
{
    pthread_mutex_lock(&account->lock);
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    ++shared;
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&account->lock);
}

static void backward(struct Account* account)
{
    pthread_mutex_lock(&account->lock);
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    ++shared;
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&account->lock);
}

static void* worker(void* argument)
{
    (void)argument;
    forward(&alice);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    backward(&bob);
    pthread_join(thread, NULL);
    return shared;
}
