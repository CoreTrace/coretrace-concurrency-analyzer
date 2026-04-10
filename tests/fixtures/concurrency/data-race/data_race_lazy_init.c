// SPDX-License-Identifier: Apache-2.0
// Test: Data race - initialisation paresseuse sans synchronisation
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    int value;
    char name[32];
} Resource;

Resource* global_resource = NULL;  // Initialisé paresseusement, non protégé

Resource* get_resource() {
    if (global_resource == NULL) {           // DATA RACE: lecture non protégée
        global_resource = malloc(sizeof(Resource));  // DATA RACE: écriture non protégée
        global_resource->value = 42;
    }
    return global_resource;
}

void* worker(void* arg) {
    Resource* r = get_resource();  // Plusieurs threads peuvent initialiser simultanément
    printf("Resource value: %d\n", r->value);
    return NULL;
}

int main() {
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) {
        pthread_create(&threads[i], NULL, worker, NULL);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }
    free(global_resource);
    return 0;
}
