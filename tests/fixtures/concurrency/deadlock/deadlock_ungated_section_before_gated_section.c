// SPDX-License-Identifier: Apache-2.0
// deadlock_gated_section_before_ungated_section.c with main's two sections swapped: without gate
// first, then under gate (#127).
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&gate);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);

    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);

    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&gate);

    pthread_join(thread, NULL);
    return 0;
}
