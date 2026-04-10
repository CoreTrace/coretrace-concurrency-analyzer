// SPDX-License-Identifier: Apache-2.0
// Test: Data race sur un flag d'arrêt non atomique
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int stop_flag = 0;  // Flag d'arrêt - doit être atomique ou protégé

void* worker(void* arg) {
    int count = 0;
    while (!stop_flag) {  // DATA RACE: lecture sans synchronisation
        count++;
    }
    printf("Worker arrêté après %d itérations\n", count);
    return NULL;
}

int main() {
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    usleep(1000);
    stop_flag = 1;  // DATA RACE: écriture sans synchronisation
    pthread_join(t, NULL);
    return 0;
}
