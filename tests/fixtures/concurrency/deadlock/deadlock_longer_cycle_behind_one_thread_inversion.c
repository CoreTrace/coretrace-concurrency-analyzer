// SPDX-License-Identifier: Apache-2.0
// The worker takes alice then bob, and later bob then alice; the auditor takes alice then hub, the
// archiver hub then bob. The worker holding bob, the auditor holding alice and the archiver holding
// hub can wait for each other in a ring (#127).
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t alice = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bob = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hub = PTHREAD_MUTEX_INITIALIZER;

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
