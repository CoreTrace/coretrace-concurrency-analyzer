// SPDX-License-Identifier: Apache-2.0
// Test: Data race - compteurs de statistiques mis à jour par un pool de threads
#include <pthread.h>
#include <stdio.h>

#define NUM_THREADS 8
#define ITERATIONS  50000

typedef struct {
    long total_requests;
    long failed_requests;
    long bytes_processed;
} Stats;

Stats global_stats = {0, 0, 0};  // Partagé entre tous les threads sans protection

void* process_requests(void* arg) {
    int id = *(int*)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        global_stats.total_requests++;   // DATA RACE: incrémentation non atomique
        if (i % 17 == 0) {
            global_stats.failed_requests++;  // DATA RACE
        }
        global_stats.bytes_processed += 512;  // DATA RACE
    }
    return NULL;
}

int main() {
    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, process_requests, &ids[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("Total: %ld, Failed: %ld, Bytes: %ld\n",
           global_stats.total_requests,
           global_stats.failed_requests,
           global_stats.bytes_processed);
    return 0;
}
