// SPDX-License-Identifier: Apache-2.0
// Test: Once-init - double-checked locking avec volatile utilisé incorrectement
//
// Tentative de "corriger" le double-checked locking en déclarant le pointeur
// volatile. volatile empêche uniquement les optimisations du compilateur —
// il ne fournit AUCUNE garantie de visibilité entre threads ni barrière mémoire.
// Un thread peut voir singleton != NULL mais observer un objet partiellement
// construit en raison du réordering CPU. Seul std::atomic ou __atomic garantit
// la sémantique correcte.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int   id;
    char  name[32];
    int   initialized_flag;
} Singleton;

// ONCE-INIT: volatile ne garantit pas l'ordre des écritures entre threads
static volatile Singleton* singleton = NULL;
static pthread_mutex_t     init_mutex = PTHREAD_MUTEX_INITIALIZER;

Singleton* get_singleton() {
    if (singleton == NULL) {              // ONCE-INIT: lecture sans verrou — visible mais
                                          // l'objet pointé peut être partiellement écrit
        pthread_mutex_lock(&init_mutex);
        if (singleton == NULL) {
            Singleton* tmp = malloc(sizeof(Singleton));
            tmp->id               = 1;
            tmp->initialized_flag = 42;   // Écritures potentiellement réordonnées par le CPU
            // ONCE-INIT: l'affectation à singleton peut être visible avant les écritures
            // dans tmp sur d'autres cœurs (pas de release fence)
            singleton = tmp;
        }
        pthread_mutex_unlock(&init_mutex);
    }
    return (Singleton*)singleton;         // Cast retirant volatile : UB supplémentaire
}

void* reader(void* arg) {
    Singleton* s = get_singleton();
    printf("id=%d flag=%d\n", s->id, s->initialized_flag);
    return NULL;
}

int main() {
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) {
        pthread_create(&threads[i], NULL, reader, NULL);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }
    free((void*)singleton);
    return 0;
}
