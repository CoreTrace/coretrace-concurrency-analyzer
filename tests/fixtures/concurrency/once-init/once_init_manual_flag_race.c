// SPDX-License-Identifier: Apache-2.0
// Test: Once-init - drapeau d'initialisation manuel sans mutex
//
// Un entier initialized = 0 est utilisé comme garde d'initialisation unique
// sans aucun verrou ni opération atomique. Plusieurs threads peuvent lire
// initialized == 0 simultanément, franchir la garde et initialiser la ressource
// partagée plusieurs fois — causant à la fois une data race et une initialisation
// multiple non déterministe.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int  initialized = 0;  // ONCE-INIT: drapeau non atomique, non protégé
static int* shared_data  = NULL;
static int  data_size    = 0;

void init_shared_data() {
    // ONCE-INIT: plusieurs threads peuvent entrer simultanément si initialized vaut 0
    if (!initialized) {
        shared_data = malloc(256 * sizeof(int));  // DATA RACE: allocation multiple possible
        data_size   = 256;
        for (int i = 0; i < 256; i++) shared_data[i] = i * 2;
        initialized = 1;  // DATA RACE: écriture non atomique visible tardivement
    }
}

void* worker(void* arg) {
    init_shared_data();   // ONCE-INIT: plusieurs threads peuvent initialiser
    if (shared_data) {
        // DATA RACE: shared_data peut pointer vers une allocation libérée ou nulle
        printf("data[0]=%d size=%d\n", shared_data[0], data_size);
    }
    return NULL;
}

int main() {
    pthread_t threads[6];
    for (int i = 0; i < 6; i++) {
        pthread_create(&threads[i], NULL, worker, NULL);
    }
    for (int i = 0; i < 6; i++) {
        pthread_join(threads[i], NULL);
    }
    free(shared_data);
    return 0;
}
