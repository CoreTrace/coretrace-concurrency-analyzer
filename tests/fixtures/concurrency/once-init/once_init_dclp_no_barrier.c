// SPDX-License-Identifier: Apache-2.0
// Test: Once-init - double-checked locking sans atomiques (pattern cassé en C)
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    int* data;
    int  size;
} Cache;

static Cache*          cache = NULL;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

Cache* get_cache() {
    if (cache == NULL) {                      // ONCE-INIT: première vérification sans verrou
        pthread_mutex_lock(&cache_mutex);
        if (cache == NULL) {                  // Deuxième vérification avec verrou
            Cache* tmp  = malloc(sizeof(Cache));
            tmp->data   = malloc(256 * sizeof(int));
            tmp->size   = 256;
            for (int i = 0; i < 256; i++) tmp->data[i] = i;
            // ONCE-INIT: sans barrière mémoire, un autre thread peut voir cache != NULL
            // mais observer tmp->data ou tmp->size non encore écrits (publication non sûre)
            cache = tmp;
        }
        pthread_mutex_unlock(&cache_mutex);
    }
    return cache;  // Peut retourner un objet partiellement initialisé
}

void* reader(void* arg) {
    Cache* c = get_cache();
    printf("Cache size: %d, data[0]: %d\n", c->size, c->data[0]);
    return NULL;
}

int main() {
    pthread_t threads[6];
    for (int i = 0; i < 6; i++) {
        pthread_create(&threads[i], NULL, reader, NULL);
    }
    for (int i = 0; i < 6; i++) {
        pthread_join(threads[i], NULL);
    }
    free(cache->data);
    free(cache);
    return 0;
}
