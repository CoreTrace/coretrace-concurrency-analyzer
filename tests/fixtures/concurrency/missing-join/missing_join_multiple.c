// SPDX-License-Identifier: Apache-2.0
// Test: Missing join multiple - plusieurs threads non joints
#include <pthread.h>
#include <stdio.h>

int results[5] = {0};

void* compute(void* arg) {
    int id = *(int*)arg;
    // Simulation de calcul
    for (volatile int i = 0; i < 10000; i++);
    results[id] = id * 10;
    printf("Thread %d completed\n", id);
    return NULL;
}

int main() {
    pthread_t threads[5];
    int ids[5] = {0, 1, 2, 3, 4};
    
    // Création de 5 threads
    for (int i = 0; i < 5; i++) {
        pthread_create(&threads[i], NULL, compute, &ids[i]);
    }
    
    // MISSING JOIN: seul le premier thread est joint
    pthread_join(threads[0], NULL);
    
    // Les 4 autres threads ne sont jamais joints!
    // Leurs résultats peuvent être incomplets ou perdus
    
    printf("Results: ");
    for (int i = 0; i < 5; i++) {
        printf("%d ", results[i]);
    }
    printf("\n");
    
    return 0;
}
