// SPDX-License-Identifier: Apache-2.0
// Test: Condition variable - signal envoyé avant le wait, notification perdue
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

pthread_mutex_t mtx  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;

void* notifier(void* arg) {
    usleep(1000);
    // CONDITION VARIABLE: signal envoyé sans vérifier si quelqu'un attend
    pthread_cond_signal(&cond);  // Notification potentiellement perdue si waiter pas encore prêt
    printf("Signal envoyé\n");
    return NULL;
}

void* waiter(void* arg) {
    usleep(5000);  // Arrive après le signal -> attend indéfiniment
    pthread_mutex_lock(&mtx);
    pthread_cond_wait(&cond, &mtx);  // LOST WAKEUP: signal déjà parti, attend pour toujours
    printf("Wakeup reçu\n");
    pthread_mutex_unlock(&mtx);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, notifier, NULL);
    pthread_create(&t2, NULL, waiter, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
