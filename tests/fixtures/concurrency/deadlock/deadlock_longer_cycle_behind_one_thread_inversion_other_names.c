// SPDX-License-Identifier: Apache-2.0
// deadlock_longer_cycle_behind_one_thread_inversion.c with the lock hub renamed ahub; nothing else
// changes (#127).
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t alice = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bob = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ahub = PTHREAD_MUTEX_INITIALIZER;

void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&alice);
    pthread_mutex_lock(&bob);
    pthread_mutex_unlock(&bob);
    pthread_mutex_unlock(&alice);

    pthread_mutex_lock(&bob);
    pthread_mutex_lock(&alice);
    pthread_mutex_unlock(&alice);
    pthread_mutex_unlock(&bob);
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
    pthread_t work;
    pthread_t audit;
    pthread_t archive;
    pthread_create(&work, NULL, worker, NULL);
    pthread_create(&audit, NULL, auditor, NULL);
    pthread_create(&archive, NULL, archiver, NULL);
    pthread_join(work, NULL);
    pthread_join(audit, NULL);
    pthread_join(archive, NULL);
    return 0;
}
