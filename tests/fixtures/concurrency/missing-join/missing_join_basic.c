// SPDX-License-Identifier: Apache-2.0
// Test: Missing join - thread non joint, ressources potentiellement perdues
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int* shared_data = NULL;

void* worker(void* arg) {
    shared_data = malloc(100 * sizeof(int));
    for (int i = 0; i < 100; i++) {
        shared_data[i] = i;
    }
    printf("Worker finished, allocated data at %p\n", (void*)shared_data);
    return NULL;
}

int main() {
    pthread_t t;
    
    pthread_create(&t, NULL, worker, NULL);
    
    // MISSING JOIN: le thread n'est jamais joint!
    // pthread_join(t, NULL);  // <-- Manque ici
    
    // La mémoire allouée dans le thread peut être perdue
    // et les résultats ne sont pas synchronisés
    if (shared_data) {
        printf("Main: data[0] = %d\n", shared_data[0]);
        // Free peut être appelé trop tôt si le thread n'a pas fini
        free(shared_data);
    }
    
    return 0;
}
