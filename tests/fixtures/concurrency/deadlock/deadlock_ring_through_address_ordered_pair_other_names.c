// SPDX-License-Identifier: Apache-2.0
// deadlock_ring_through_address_ordered_pair.c with the lock hub renamed ahub; nothing else changes
// (#138).
// Expected: one deadlock, the ring.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t alice = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bob = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ahub = PTHREAD_MUTEX_INITIALIZER;

void lockPairByAddress(void)
{
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
    pthread_mutex_unlock(&bob);
    pthread_mutex_unlock(&alice);
}

void* pairWorker(void* argument)
{
    (void)argument;
    lockPairByAddress();
    return NULL;
}

void* auditor(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&alice);
    pthread_mutex_lock(&ahub);
    pthread_mutex_unlock(&ahub);
    pthread_mutex_unlock(&alice);
    return NULL;
}

void* archiver(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&ahub);
    pthread_mutex_lock(&bob);
    pthread_mutex_unlock(&bob);
    pthread_mutex_unlock(&ahub);
    return NULL;
}

int main(void)
{
    pthread_t pair;
    pthread_t audit;
    pthread_t archive;
    pthread_create(&pair, NULL, pairWorker, NULL);
    pthread_create(&audit, NULL, auditor, NULL);
    pthread_create(&archive, NULL, archiver, NULL);
    lockPairByAddress();
    pthread_join(pair, NULL);
    pthread_join(audit, NULL);
    pthread_join(archive, NULL);
    return 0;
}
