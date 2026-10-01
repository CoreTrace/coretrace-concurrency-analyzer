// SPDX-License-Identifier: Apache-2.0
// deadlock_gate_taken_through_parameter_no_diagnostic.c with the helpers taking alice's lock by
// name instead of through their parameter (#136).
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

static void forward(struct Account* account)
{
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&alice.lock);
}

static void backward(struct Account* account)
{
    pthread_mutex_lock(&alice.lock);
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&alice.lock);
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
    backward(&alice);
    pthread_join(thread, NULL);
    return 0;
}
