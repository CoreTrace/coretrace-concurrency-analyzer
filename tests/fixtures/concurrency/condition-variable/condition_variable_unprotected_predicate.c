// SPDX-License-Identifier: Apache-2.0
// The producer publishes the state without taking the mutex, while the consumer reads it
// under the mutex. Signalling under the lock does not make the publication safe: the two
// sides share no common lock on the state itself.
//
// Unlike the spurious-wakeup bug, this one is a plain data race and must be reported by the
// existing rule. It pins that the analyzer still sees conflicts around a condition variable.
#include <pthread.h>
#include <stdbool.h>

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

bool data_ready = false;
int shared_data = 0;

void* producer(void* arg)
{
    (void)arg;
    shared_data = 42;
    data_ready = true;

    pthread_mutex_lock(&mutex);
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
