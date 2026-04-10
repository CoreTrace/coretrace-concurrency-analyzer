// SPDX-License-Identifier: Apache-2.0
// Test: Deadlock - thread attend sur condvar tout en tenant un autre verrou
#include <pthread.h>
#include <stdio.h>

pthread_mutex_t mutex_a = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_b = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond    = PTHREAD_COND_INITIALIZER;
int ready = 0;

void* waiter(void* arg) {
    pthread_mutex_lock(&mutex_a);      // Acquiert A
    pthread_mutex_lock(&mutex_b);      // Acquiert B (DEADLOCK: notifier essaie d'acquérir B)
    while (!ready) {
        pthread_cond_wait(&cond, &mutex_b);  // Attend sur cond en tenant A ET B
    }
    pthread_mutex_unlock(&mutex_b);
    pthread_mutex_unlock(&mutex_a);
    return NULL;
}

void* notifier(void* arg) {
    pthread_mutex_lock(&mutex_b);  // DEADLOCK: mutex_b tenu par waiter
    ready = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex_b);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, waiter, NULL);
    pthread_create(&t2, NULL, notifier, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
