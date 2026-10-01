// SPDX-License-Identifier: Apache-2.0
// Deadlock when bob sits below alice in memory, which the program does not control. main and a
// worker lock alice and bob lower address first; the auditor takes alice then hub, the archiver
// hub then bob. Holding bob, alice and hub respectively, a pair thread, the auditor and the
// archiver can wait for each other in a ring. The pair alone is no deadlock (#138).
// Expected: one deadlock, the ring.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t alice = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bob = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hub = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&hub);
    pthread_mutex_unlock(&hub);
    pthread_mutex_unlock(&alice);
    return NULL;
}

void* archiver(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&hub);
    pthread_mutex_lock(&bob);
    pthread_mutex_unlock(&bob);
    pthread_mutex_unlock(&hub);
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
