// SPDX-License-Identifier: Apache-2.0
// Test: Thread-local - pointeur global vers une variable TLS, lu après terminaison du thread
//
// Un thread stocke l'adresse de sa variable thread-locale dans un pointeur global.
// Après la terminaison du thread, le stockage TLS est libéré par le runtime.
// Le thread principal lit le pointeur global et déréférence l'adresse maintenant
// invalide : use-after-free sur la mémoire TLS du thread terminé.
#include <pthread.h>
#include <stdio.h>

static _Thread_local int tls_value = 0;
static int* shared_ptr = NULL;  // Contiendra l'adresse de tls_value du worker

void* worker(void* arg) {
    tls_value = 99;
    // USE-AFTER-FREE: publie l'adresse de la TLS dans le global partagé.
    // Cette adresse devient invalide à la terminaison du thread.
    shared_ptr = &tls_value;
    return NULL;
}

int main() {
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_join(t, NULL);  // Le thread est terminé — sa TLS est libérée

    // USE-AFTER-FREE: déréférence l'adresse TLS d'un thread mort
    if (shared_ptr != NULL) {
        printf("tls_value = %d\n", *shared_ptr);  // USE-AFTER-FREE
        *shared_ptr = 0;                           // USE-AFTER-FREE: écriture sur TLS libérée
    }
    return 0;
}
