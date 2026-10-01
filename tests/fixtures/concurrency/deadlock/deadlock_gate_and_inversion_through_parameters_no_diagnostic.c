// SPDX-License-Identifier: Apache-2.0
// deadlock_gate_taken_through_parameter_no_diagnostic.c with first and second also handed to the
// helpers: the inversion is then between two parameter locks, and alice's lock is still held around
// both orders (#136).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account alice = {PTHREAD_MUTEX_INITIALIZER, 100};
static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void forward(struct Account* account, pthread_mutex_t* one, pthread_mutex_t* two)
{
    pthread_mutex_lock(&account->lock);
    pthread_mutex_lock(one);
    pthread_mutex_lock(two);
    pthread_mutex_unlock(two);
    pthread_mutex_unlock(one);
    pthread_mutex_unlock(&account->lock);
}

static void backward(struct Account* account, pthread_mutex_t* one, pthread_mutex_t* two)
{
    pthread_mutex_lock(&account->lock);
    pthread_mutex_lock(two);
    pthread_mutex_lock(one);
    pthread_mutex_unlock(one);
    pthread_mutex_unlock(two);
    pthread_mutex_unlock(&account->lock);
}

static void* worker(void* argument)
{
    (void)argument;
    forward(&alice, &first, &second);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    backward(&alice, &first, &second);
    pthread_join(thread, NULL);
    return 0;
}
