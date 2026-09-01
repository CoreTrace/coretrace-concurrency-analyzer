// SPDX-License-Identifier: Apache-2.0
// Correct counterpart of condition_variable_spurious.c: the predicate is rechecked in a
// loop, so a spurious wakeup simply waits again. Every access to the shared state happens
// under the mutex, so no rule has anything to report.
//
// This fixture is the negative case a future spurious-wakeup rule must not flag: the only
// structural difference with the buggy version is the back edge around pthread_cond_wait.
#include <pthread.h>
#include <stdbool.h>

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

bool data_ready = false;
int shared_data = 0;

void* producer(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&mutex);
    shared_data = 42;
    data_ready = true;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
    return NULL;
}

void* consumer(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&mutex);

    while (!data_ready)
        pthread_cond_wait(&cond, &mutex);

    const int value = shared_data;
    (void)value;

    pthread_mutex_unlock(&mutex);
    return NULL;
}

int main(void)
{
    pthread_t first, second;

    pthread_create(&first, NULL, consumer, NULL);
    pthread_create(&second, NULL, producer, NULL);

    pthread_join(first, NULL);
    pthread_join(second, NULL);

    return 0;
}
