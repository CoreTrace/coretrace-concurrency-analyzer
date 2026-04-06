// SPDX-License-Identifier: Apache-2.0
// Test 12: Condition variable - attente sans boucle (spurious wakeup)
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

bool data_ready = false;
int shared_data = 0;

void* producer(void* arg) {
    pthread_mutex_lock(&mutex);
    shared_data = 42;
    data_ready = true;
    pthread_cond_signal(&cond);  // Signal avant release
    pthread_mutex_unlock(&mutex);
    return NULL;
}

void* consumer(void* arg) {
    pthread_mutex_lock(&mutex);

    // MAUVAIS: pas de boucle while pour gérer les spurious wakeups
    if (!data_ready) {  // devrait être: while (!data_ready)
        pthread_cond_wait(&cond, &mutex);
    }

    // Peut se réveiller même si data_ready est encore false!
    int value = shared_data;
    printf("Consumer got: %d\n", value);

    pthread_mutex_unlock(&mutex);
    return NULL;
}

int main(void)
{
    pthread_t t1, t2;

    pthread_create(&t1, NULL, consumer, NULL);
    pthread_create(&t2, NULL, producer, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    return 0;
}
