// SPDX-License-Identifier: Apache-2.0
// deadlock_address_ordered_pair_entered_and_left_no_diagnostic.c without the helper: each thread
// locks alice and bob lower address first, then ledger; main holds registry around it (#138).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t alice = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bob = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ledger = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* argument)
{
    (void)argument;
    if (&alice < &bob)
    {
        pthread_mutex_lock(&alice);
        pthread_mutex_lock(&bob);
    }
    else
    {
        pthread_mutex_lock(&bob);
        pthread_mutex_lock(&alice);
    }
    pthread_mutex_lock(&ledger);
    pthread_mutex_unlock(&ledger);
    pthread_mutex_unlock(&bob);
    pthread_mutex_unlock(&alice);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_mutex_lock(&registry);
    if (&alice < &bob)
    {
        pthread_mutex_lock(&alice);
        pthread_mutex_lock(&bob);
    }
    else
    {
        pthread_mutex_lock(&bob);
        pthread_mutex_lock(&alice);
    }
    pthread_mutex_lock(&ledger);
    pthread_mutex_unlock(&ledger);
    pthread_mutex_unlock(&bob);
    pthread_mutex_unlock(&alice);
    pthread_mutex_unlock(&registry);
    pthread_join(thread, NULL);
    return 0;
}
