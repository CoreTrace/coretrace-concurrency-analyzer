// SPDX-License-Identifier: Apache-2.0
// deadlock_pair_taken_before_and_after_spawn.c with the lock `first` renamed `third`; nothing else
// changes (#127).
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t third = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&third);
    pthread_mutex_unlock(&third);
    pthread_mutex_unlock(&second);
    return NULL;
}

int main(void)
{
    pthread_mutex_lock(&third);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&third);

    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);

    pthread_mutex_lock(&third);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&third);
    pthread_join(thread, NULL);
    return 0;
}
